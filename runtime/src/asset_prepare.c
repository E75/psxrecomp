/* In-exe asset preparation -- see asset_prepare.h and docs/IN_EXE_ASSETS.md.
 *
 * The only artefact built here is the raw STR extent (MOVIE/WE2002.STR): a
 * straight 2352-byte sector copy behind the PSTR header audio_native.c reads.
 * The ISO9660 walk, cue parsing and sector layout mirror
 * tools/assets/extract_iso.py + extract_str_raw.py.
 *
 * The HD texture pack (Konami TEX/DAT2D codec) and the MODEL.BIN override
 * (mesh patcher) need the Python tools; they are detected and reported, never
 * reimplemented here.
 */
#include "asset_prepare.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/types.h>
#endif

#define AP_SECTOR_RAW    2352u
#define AP_SECTOR_COOKED 2048u
#define AP_STR_HEADER    32u
#define AP_MAX_DEPTH     8

typedef struct ApDisc {
    FILE*     f;
    long long bytes;
    unsigned  sector; /* 2352 or 2048 */
    int       raw;
} ApDisc;

typedef struct ApExtent {
    unsigned lba;
    unsigned size;
} ApExtent;

typedef struct ApState {
    char      root[ASSET_PREPARE_PATH_MAX];
    char      assets_dir[ASSET_PREPARE_PATH_MAX];
    char      str_dir[ASSET_PREPARE_PATH_MAX];
    char      str_path[ASSET_PREPARE_PATH_MAX];
    char      report_path[ASSET_PREPARE_PATH_MAX];
    char      disc[ASSET_PREPARE_PATH_MAX];
    char      note[256];
    int       str_status;   /* 0 skipped, 1 produced, 2 present, 3 failed */
    int       str_valid;
    long long str_bytes;
    unsigned  str_lba;
    unsigned  str_sectors;
    unsigned  str_video;
    unsigned  str_audio;
    unsigned  str_other;
    int       str_first_audio;
    int       audio_present;
    int       hd_present;
    int       override_present;
} ApState;

static char g_last_error[256];

const char* asset_prepare_last_error(void) {
    return g_last_error;
}

static void ap_error(const char* msg) {
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg ? msg : "error");
}

/* --------------------------------------------------------------- path/fs */

static int ap_file_size(const char* path, long long* out) {
    FILE* f = fopen(path, "rb");
    long long n;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    n = ftell(f);
    fclose(f);
    if (n < 0) return -1;
    if (out) *out = n;
    return 0;
}

static int ap_is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

static int ap_make_dir(const char* path) {
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

static int ap_ensure_dir(const char* path) {
    if (ap_is_dir(path)) return 0;
    if (ap_make_dir(path) == 0) return 0;
    return ap_is_dir(path) ? 0 : -1;
}

static void ap_join(char* out, size_t cap, const char* a, const char* b) {
    if (!a || !a[0]) {
        snprintf(out, cap, "%s", b ? b : "");
        return;
    }
    if (!b || !b[0]) {
        snprintf(out, cap, "%s", a);
        return;
    }
    const size_t n = strlen(a);
    const int sep = (a[n - 1] == '/' || a[n - 1] == '\\') ? 0 : 1;
    snprintf(out, cap, "%s%s%s", a, sep ? "/" : "", b);
}

static void ap_dirname(char* out, size_t cap, const char* path) {
    const char* slash = NULL;
    const char* p;
    for (p = path; p && *p; ++p)
        if (*p == '/' || *p == '\\') slash = p;
    if (!slash) {
        snprintf(out, cap, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0) n = 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static int ap_ci_ncmp(const char* a, const char* b, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
    return 0;
}

static int ap_ci_equal(const char* a, const char* b) {
    return ap_ci_ncmp(a, b, strlen(a) + 1) == 0;
}

static int ap_ci_suffix(const char* path, const char* suffix) {
    const size_t n = strlen(path), m = strlen(suffix);
    if (n < m) return 0;
    return ap_ci_ncmp(path + n - m, suffix, m) == 0;
}

static int ap_seek(FILE* f, long long off, int whence) {
#if defined(_WIN32)
    return _fseeki64(f, off, whence);
#else
    return fseeko(f, (off_t)off, whence);
#endif
}

/* Replace an existing file. C rename() does not overwrite on Windows, so the
 * report would go stale after the first run without this. */
static int ap_replace_file(const char* tmp, const char* dst) {
#if defined(_WIN32)
    remove(dst);
#endif
    return rename(tmp, dst) == 0 ? 0 : -1;
}

static unsigned ap_le32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

static void ap_put_le32(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* -------------------------------------------------------------- cue/disc */

/* First BINARY / MODE1-2352 / MODE2-2352 FILE of the sheet, resolved next to
 * it. Same selection rule as tools/assets/extract_iso.py find_track_image(). */
static int ap_cue_track_image(const char* cue, char* out, size_t cap) {
    FILE* f = fopen(cue, "rb");
    char line[1024];
    char dir[ASSET_PREPARE_PATH_MAX];
    int found = 0;
    if (!f) return 0;
    ap_dirname(dir, sizeof(dir), cue);
    while (!found && fgets(line, sizeof(line), f)) {
        char* p = line;
        char name[ASSET_PREPARE_PATH_MAX];
        char kind[32];
        size_t n;
        while (*p == ' ' || *p == '\t') ++p;
        if (ap_ci_ncmp(p, "FILE", 4) != 0) continue;
        p += 4;
        if (*p != ' ' && *p != '\t') continue;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '"') {
            char* end;
            ++p;
            end = strchr(p, '"');
            if (!end) continue;
            n = (size_t)(end - p);
            if (n >= sizeof(name)) continue;
            memcpy(name, p, n);
            name[n] = '\0';
            p = end + 1;
        } else {
            n = strcspn(p, " \t\r\n");
            if (n == 0 || n >= sizeof(name)) continue;
            memcpy(name, p, n);
            name[n] = '\0';
            p += n;
        }
        while (*p == ' ' || *p == '\t') ++p;
        n = strcspn(p, " \t\r\n");
        if (n == 0 || n >= sizeof(kind)) continue;
        memcpy(kind, p, n);
        kind[n] = '\0';
        if (!(ap_ci_equal(kind, "BINARY") || ap_ci_equal(kind, "MODE1/2352") ||
              ap_ci_equal(kind, "MODE2/2352")))
            continue;
        if (name[0] == '/' || name[0] == '\\' ||
            (name[0] && name[1] == ':')) {
            snprintf(out, cap, "%s", name);
        } else {
            ap_join(out, cap, dir, name);
        }
        if (ap_file_size(out, NULL) == 0) found = 1;
    }
    fclose(f);
    return found;
}

static int ap_disc_open(ApDisc* d, const char* path, char* err, size_t err_cap) {
    long long size = 0;
    unsigned char first = 0;
    memset(d, 0, sizeof(*d));
    if (ap_file_size(path, &size) != 0 || size <= 0) {
        snprintf(err, err_cap, "disc image not found: %s", path);
        return 0;
    }
    d->f = fopen(path, "rb");
    if (!d->f) {
        snprintf(err, err_cap, "cannot open disc image: %s", path);
        return 0;
    }
    d->bytes = size;
    if (fread(&first, 1, 1, d->f) != 1) first = 0xFF;
    if (size % AP_SECTOR_RAW == 0 && first == 0) {
        d->raw = 1;
        d->sector = AP_SECTOR_RAW;
    } else if (size % AP_SECTOR_COOKED == 0) {
        d->raw = 0;
        d->sector = AP_SECTOR_COOKED;
    } else {
        snprintf(err, err_cap,
                 "%s: size %lld is neither 2352 nor 2048 aligned", path, size);
        fclose(d->f);
        d->f = NULL;
        return 0;
    }
    return 1;
}

static void ap_disc_close(ApDisc* d) {
    if (d->f) fclose(d->f);
    d->f = NULL;
}

/* User data of one sector. Returns bytes copied (<= cap); *payload is the
 * sector's full user-data size (2048 Form 1, 2324 Form 2). */
static unsigned ap_disc_read_sector(ApDisc* d, unsigned lba,
                                    unsigned char* out, unsigned cap,
                                    unsigned* payload) {
    unsigned char head[24];
    unsigned off = 0, pl = d->sector;
    const long long base = (long long)lba * (long long)d->sector;
    unsigned n;
    int sync_ok = 1;
    unsigned i;
    if (!d->f) return 0;
    if (d->raw) {
        if (base + 24 > d->bytes) return 0;
        if (ap_seek(d->f, base, SEEK_SET) != 0) return 0;
        if (fread(head, 1, 24, d->f) != 24) return 0;
        if (head[0] != 0) sync_ok = 0;
        for (i = 1; i < 11; ++i)
            if (head[i] != 0xFF) sync_ok = 0;
        if (!sync_ok) return 0;
        if (head[15] == 1) {
            off = 16;
            pl = AP_SECTOR_COOKED;
        } else if (head[15] == 2) {
            off = 24;
            pl = (head[18] & 0x20) ? 2324u : AP_SECTOR_COOKED;
        } else {
            return 0;
        }
    }
    if (base + (long long)off + (long long)pl > d->bytes) return 0;
    if (ap_seek(d->f, base + (long long)off, SEEK_SET) != 0) return 0;
    n = pl < cap ? pl : cap;
    if (n == 0 || fread(out, 1, n, d->f) != n) return 0;
    if (payload) *payload = pl;
    return n;
}

/* ------------------------------------------------------------ ISO9660 */

static int ap_walk_dir(ApDisc* d, unsigned lba, unsigned size,
                       const char* prefix, int depth, const char* want,
                       ApExtent* out) {
    unsigned sectors, s;
    unsigned char buf[AP_SECTOR_COOKED];
    if (depth > AP_MAX_DEPTH) return 0;
    sectors = (size + AP_SECTOR_COOKED - 1) / AP_SECTOR_COOKED;
    if (sectors == 0) sectors = 1;
    for (s = 0; s < sectors; ++s) {
        const unsigned got =
            ap_disc_read_sector(d, lba + s, buf, sizeof(buf), NULL);
        unsigned pos = 0;
        while (pos + 33 <= got) {
            const unsigned len = buf[pos];
            const unsigned char* rec;
            unsigned nlen;
            if (len == 0) break; /* records never span a sector */
            if (len < 33 || pos + len > got) break;
            rec = buf + pos;
            nlen = rec[32];
            if (rec[1] == 0 && nlen > 0 && pos + 33 + nlen <= got &&
                !(nlen == 1 && (rec[33] == 0 || rec[33] == 1))) {
                char name[256];
                char full[512];
                unsigned n = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
                char* semi;
                memcpy(name, rec + 33, n);
                name[n] = '\0';
                semi = strchr(name, ';');
                if (semi) *semi = '\0';
                snprintf(full, sizeof(full), "%s%s", prefix, name);
                if (rec[25] & 0x02) {
                    char sub[512];
                    snprintf(sub, sizeof(sub), "%s/", full);
                    if (ap_walk_dir(d, ap_le32(rec + 2), ap_le32(rec + 10),
                                    sub, depth + 1, want, out))
                        return 1;
                } else if (ap_ci_equal(full, want)) {
                    out->lba = ap_le32(rec + 2);
                    out->size = ap_le32(rec + 10);
                    return 1;
                }
            }
            pos += len;
        }
    }
    return 0;
}

static int ap_find_entry(ApDisc* d, const char* want, ApExtent* out) {
    unsigned char pvd[AP_SECTOR_COOKED];
    unsigned got;
    const unsigned char* root;
    if (ap_disc_read_sector(d, 16, pvd, sizeof(pvd), &got) == 0 || got < 190)
        return 0;
    if (memcmp(pvd + 1, "CD001", 5) != 0) return 0;
    root = pvd + 156;
    return ap_walk_dir(d, ap_le32(root + 2), ap_le32(root + 10), "", 0, want,
                       out);
}

/* ------------------------------------------------------------- artefacts */

static int ap_str_raw_check(const char* path, long long* bytes_out) {
    unsigned char hdr[AP_STR_HEADER];
    FILE* f;
    long long size = 0;
    unsigned sectors, ssize;
    int ok = 0;
    if (ap_file_size(path, &size) != 0) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
        memcmp(hdr, "PSTR", 4) == 0) {
        sectors = ap_le32(hdr + 12);
        ssize = ap_le32(hdr + 16);
        if (ap_le32(hdr + 4) == 1u && ssize == AP_SECTOR_RAW && sectors > 0) {
            if (size ==
                (long long)AP_STR_HEADER + (long long)sectors * AP_SECTOR_RAW)
                ok = 1;
        }
    }
    fclose(f);
    if (bytes_out) *bytes_out = size;
    return ok;
}

static int ap_write_str_raw(ApDisc* d, const ApExtent* e, const char* path,
                            ApState* st, char* err, size_t err_cap) {
    unsigned char hdr[AP_STR_HEADER];
    unsigned char sec[AP_SECTOR_RAW];
    char tmp[ASSET_PREPARE_PATH_MAX];
    FILE* f;
    unsigned sectors, i;
    long long total;
    if (!d->raw) {
        snprintf(err, err_cap,
                 "cooked 2048-byte image: Form 2 payload (2324 B) needs the "
                 "raw 2352-byte track");
        return 0;
    }
    sectors = (e->size + AP_SECTOR_COOKED - 1) / AP_SECTOR_COOKED;
    total = (long long)(e->lba + sectors) * AP_SECTOR_RAW;
    if (sectors == 0 || total > d->bytes) {
        snprintf(err, err_cap, "extent %u+%u is past the image (%lld bytes)",
                 e->lba, sectors, d->bytes);
        return 0;
    }
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "PSTR", 4);
    ap_put_le32(hdr + 4, 1u);
    ap_put_le32(hdr + 8, e->lba);
    ap_put_le32(hdr + 12, sectors);
    ap_put_le32(hdr + 16, AP_SECTOR_RAW);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, err_cap, "cannot create %s", tmp);
        return 0;
    }
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) goto fail;
    st->str_video = st->str_audio = st->str_other = 0;
    st->str_first_audio = -1;
    for (i = 0; i < sectors; ++i) {
        const unsigned long long base =
            (unsigned long long)(e->lba + i) * AP_SECTOR_RAW;
        int sync_ok = 1;
        unsigned k;
        if (ap_seek(d->f, (long long)base, SEEK_SET) != 0) goto fail;
        if (fread(sec, 1, sizeof(sec), d->f) != sizeof(sec)) goto fail;
        for (k = 1; k < 11; ++k)
            if (sec[k] != 0xFF) sync_ok = 0;
        if (sec[0] != 0 || !sync_ok || sec[15] != 2) {
            st->str_other++;
        } else if (sec[18] & 0x20) {
            if (st->str_first_audio < 0) st->str_first_audio = (int)i;
            st->str_audio++;
        } else if (sec[18] & 0x40) {
            st->str_video++;
        } else {
            st->str_other++;
        }
        if (fwrite(sec, 1, sizeof(sec), f) != sizeof(sec)) goto fail;
    }
    if (fclose(f) != 0) {
        f = NULL;
        goto fail;
    }
    f = NULL;
    if (rename(tmp, path) != 0) {
        snprintf(err, err_cap, "cannot rename %s to %s", tmp, path);
        remove(tmp);
        return 0;
    }
    st->str_lba = e->lba;
    st->str_sectors = sectors;
    st->str_bytes = (long long)AP_STR_HEADER + (long long)sectors * AP_SECTOR_RAW;
    return 1;
fail:
    if (f) fclose(f);
    remove(tmp);
    snprintf(err, err_cap, "write failed for %s", path);
    return 0;
}

/* ---------------------------------------------------------------- report */

static void ap_json_str(FILE* f, const char* s) {
    const unsigned char* p = (const unsigned char*)(s ? s : "");
    fputc('"', f);
    for (; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", *p);
            else fputc(*p, f);
        }
    }
    fputc('"', f);
}

static const char* ap_status_name(int status) {
    switch (status) {
    case 1: return "produced";
    case 2: return "present";
    case 3: return "failed";
    default: return "skipped";
    }
}

static void ap_command(char* out, size_t cap, const char* fmt,
                       const char* disc) {
    snprintf(out, cap, fmt, disc && disc[0] ? disc : "<disc.cue>");
}

static int ap_write_report(ApState* st) {
    char tmp[ASSET_PREPARE_PATH_MAX];
    char cmd[ASSET_PREPARE_PATH_MAX];
    FILE* f;
    if (ap_ensure_dir(st->assets_dir) != 0) return 0;
    snprintf(tmp, sizeof(tmp), "%s.tmp", st->report_path);
    f = fopen(tmp, "wb");
    if (!f) return 0;
    fputs("{\n \"format\": \"we2002-asset-prepare/1\",\n", f);
    fputs(" \"root\": ", f);
    ap_json_str(f, st->root);
    fputs(",\n \"disc\": ", f);
    ap_json_str(f, st->disc);
    fputs(",\n \"in_exe\": \"str_raw\",\n \"items\": [\n", f);

    fputs("  {\"name\": \"str_raw\", \"status\": ", f);
    ap_json_str(f, ap_status_name(st->str_status));
    fputs(", \"path\": ", f);
    ap_json_str(f, st->str_path);
    if (st->str_status == 1 || st->str_status == 2) {
        fprintf(f, ", \"bytes\": %lld, \"valid\": %s", st->str_bytes,
                st->str_valid ? "true" : "false");
    }
    if (st->str_status == 1) {
        fprintf(f,
                ", \"lba\": %u, \"sectors\": %u, \"video\": %u, \"audio\": %u,"
                " \"other\": %u, \"first_audio\": %d",
                st->str_lba, st->str_sectors, st->str_video, st->str_audio,
                st->str_other, st->str_first_audio);
    }
    if (st->str_status != 1 && st->str_status != 2) {
        fputs(", \"reason\": ", f);
        ap_json_str(f, st->note);
    }
    fputs("},\n", f);

    fputs("  {\"name\": \"preload_tree\", \"status\": \"skipped\", "
          "\"reason\": \"the port reads data sectors from the mounted disc "
          "image (asset_provider); no extracted tree is needed\"},\n",
          f);

    fputs("  {\"name\": \"audio_wavs\", \"status\": ", f);
    ap_json_str(f, st->audio_present ? "present" : "missing");
    fputs(", \"path\": ", f);
    {
        char p[ASSET_PREPARE_PATH_MAX];
        ap_join(p, sizeof(p), st->assets_dir, "audio");
        ap_json_str(f, p);
    }
    if (!st->audio_present) {
        ap_command(cmd, sizeof(cmd),
                   "python tools/assets/extract_xa.py --cue \"%s\" --out "
                   "assets/audio", st->disc);
        fputs(", \"reason\": \"CD-DA/XA decode needs the Python extractor; the "
              "port falls back to the mounted disc\", \"command\": ", f);
        ap_json_str(f, cmd);
    }
    fputs("},\n", f);

    fputs("  {\"name\": \"hd_pack\", \"status\": ", f);
    ap_json_str(f, st->hd_present ? "present" : "missing");
    fputs(", \"path\": ", f);
    {
        char p[ASSET_PREPARE_PATH_MAX];
        ap_join(p, sizeof(p), st->assets_dir, "hd_pack/pack.json");
        ap_json_str(f, p);
    }
    if (!st->hd_present) {
        ap_command(cmd, sizeof(cmd),
                   "python tools/assets/make_hd_pack.py --root runs/iso_full "
                   "--vram runs/texcodec --out assets/hd_pack", st->disc);
        fputs(", \"reason\": \"Konami TEX/DAT2D codec + VRAM palette dumps are "
              "not ported to C\", \"command\": ", f);
        ap_json_str(f, cmd);
    }
    fputs("},\n", f);

    fputs("  {\"name\": \"model_override\", \"status\": ", f);
    ap_json_str(f, st->override_present ? "present" : "missing");
    fputs(", \"path\": ", f);
    {
        char p[ASSET_PREPARE_PATH_MAX];
        ap_join(p, sizeof(p), st->assets_dir, "override/manifest.json");
        ap_json_str(f, p);
    }
    if (!st->override_present) {
        ap_command(cmd, sizeof(cmd),
                   "python tools/assets/upgrade_model.py fix-all --root "
                   "runs/iso_full --override assets/override --no-render",
                   st->disc);
        fputs(", \"reason\": \"mesh patcher (subdivide/UV repair) is not ported "
              "to C\", \"command\": ", f);
        ap_json_str(f, cmd);
    }
    fputs("}\n ],\n", f);

    ap_command(cmd, sizeof(cmd),
               "python tools/assets/prepare_assets.py --disc \"%s\"", st->disc);
    fputs(" \"python_fallback\": ", f);
    ap_json_str(f, cmd);
    fputs("\n}\n", f);

    if (fclose(f) != 0) {
        remove(tmp);
        return 0;
    }
    if (ap_replace_file(tmp, st->report_path) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ run */

int asset_prepare_run(const AssetPrepareOptions* opt, AssetPrepareResult* out) {
    ApState st;
    ApDisc disc;
    ApExtent entry;
    char track[ASSET_PREPARE_PATH_MAX];
    char err[256];
    int wrote_report = 0;
    int anything_missing;
    int rc = 0;

    memset(&st, 0, sizeof(st));
    memset(&disc, 0, sizeof(disc));
    if (out) memset(out, 0, sizeof(*out));
    g_last_error[0] = '\0';

    snprintf(st.root, sizeof(st.root), "%s",
             (opt && opt->root_dir && opt->root_dir[0]) ? opt->root_dir : ".");
    snprintf(st.disc, sizeof(st.disc), "%s",
             (opt && opt->disc_path) ? opt->disc_path : "");
    ap_join(st.assets_dir, sizeof(st.assets_dir), st.root, "assets");
    ap_join(st.str_dir, sizeof(st.str_dir), st.assets_dir, "str");
    ap_join(st.str_path, sizeof(st.str_path), st.str_dir, "WE2002.STR.raw");
    ap_join(st.report_path, sizeof(st.report_path), st.assets_dir,
            "prepare_report.json");
    if (out) {
        snprintf(out->str_raw_path, sizeof(out->str_raw_path), "%s",
                 st.str_path);
        snprintf(out->report_path, sizeof(out->report_path), "%s",
                 st.report_path);
    }

    /* What exists today. Existing artefacts are never rewritten. */
    st.str_status = 2;
    st.str_bytes = 0;
    st.str_valid = ap_str_raw_check(st.str_path, &st.str_bytes);
    if (ap_file_size(st.str_path, NULL) != 0) {
        st.str_status = 0;
        st.str_bytes = 0;
        st.str_valid = 0;
    }
    {
        char p[ASSET_PREPARE_PATH_MAX];
        ap_join(p, sizeof(p), st.assets_dir, "audio");
        st.audio_present = ap_is_dir(p) ? 1 : 0;
        ap_join(p, sizeof(p), st.assets_dir, "hd_pack/pack.json");
        st.hd_present = ap_file_size(p, NULL) == 0;
        ap_join(p, sizeof(p), st.assets_dir, "override/manifest.json");
        st.override_present = ap_file_size(p, NULL) == 0;
    }

    anything_missing = (st.str_status == 0) || !st.audio_present ||
                       !st.hd_present || !st.override_present;
    if (out) {
        out->present = (st.str_status == 2 ? 1 : 0) + st.audio_present +
                       st.hd_present + st.override_present;
        out->missing_python = (st.audio_present ? 0 : 1) +
                              (st.hd_present ? 0 : 1) +
                              (st.override_present ? 0 : 1);
    }
    /* No disc at all (BIOS-only boot, launcher-less target): stay a strict
     * no-op unless --prepare-assets asked for the report. */
    if (!st.disc[0] && !(opt && opt->force)) return 0;
    if (!anything_missing && ap_file_size(st.report_path, NULL) == 0 &&
        !(opt && opt->force)) {
        return 0; /* complete: stay a no-op, exactly like before this module */
    }

    /* ---- raw STR extent (the only in-exe artefact) --------------------- */
    if (st.str_status == 0) {
        if (!st.disc[0]) {
            snprintf(st.note, sizeof(st.note),
                     "no disc image supplied; run with --disc <cue|bin>");
            st.str_status = 3;
        } else {
            if (ap_ci_suffix(st.disc, ".cue")) {
                if (ap_file_size(st.disc, NULL) != 0) {
                    snprintf(st.note, sizeof(st.note),
                             "disc image not found: %s", st.disc);
                    st.str_status = 3;
                } else if (!ap_cue_track_image(st.disc, track, sizeof(track))) {
                    snprintf(st.note, sizeof(st.note),
                             "cue sheet names no usable data track");
                    st.str_status = 3;
                }
            } else {
                snprintf(track, sizeof(track), "%s", st.disc);
            }
            if (st.str_status != 3 && ap_disc_open(&disc, track, err,
                                                   sizeof(err))) {
                if (!ap_find_entry(&disc, "MOVIE/WE2002.STR", &entry)) {
                    snprintf(st.note, sizeof(st.note),
                             "MOVIE/WE2002.STR not in the ISO9660 tree");
                    st.str_status = 3;
                } else if (ap_ensure_dir(st.assets_dir) != 0 ||
                           ap_ensure_dir(st.str_dir) != 0) {
                    snprintf(st.note, sizeof(st.note),
                             "cannot create %s", st.str_dir);
                    st.str_status = 3;
                } else if (ap_write_str_raw(&disc, &entry, st.str_path, &st,
                                            err, sizeof(err))) {
                    st.str_status = 1;
                    st.str_valid = 1;
                } else {
                    snprintf(st.note, sizeof(st.note), "%s", err);
                    st.str_status = 3;
                }
                ap_disc_close(&disc);
            } else if (st.str_status != 3) {
                snprintf(st.note, sizeof(st.note), "%s", err);
                st.str_status = 3;
            }
        }
    }

    /* The report (and its one-line-per-missing-artefact log) is written on the
     * first run, when something was produced, or when --prepare-assets asked;
     * a later boot with nothing to do stays completely silent. */
    if ((opt && opt->force) || st.str_status == 1 ||
        ap_file_size(st.report_path, NULL) != 0) {
        wrote_report = ap_write_report(&st);
        if (!wrote_report) {
            ap_error("cannot write assets/prepare_report.json");
            return -1;
        }
    }

    /* One line per artefact that still needs the Python chain. */
    if (wrote_report) {
        if (st.str_status == 1) {
            fprintf(stdout,
                    "psxrecomp: assets: wrote %s (%lld bytes, LBA %u, %u "
                    "sectors; video=%u audio=%u)\n",
                    st.str_path, st.str_bytes, st.str_lba, st.str_sectors,
                    st.str_video, st.str_audio);
        } else if (st.str_status == 3) {
            fprintf(stdout, "psxrecomp: assets: raw STR not produced (%s)\n",
                    st.note);
        }
        if (!st.hd_present) {
            fprintf(stdout,
                    "psxrecomp: assets: HD texture pack missing "
                    "(assets/hd_pack/pack.json); needs the Konami codec: "
                    "python tools/assets/make_hd_pack.py --root runs/iso_full "
                    "--vram runs/texcodec --out assets/hd_pack\n");
        }
        if (!st.override_present) {
            fprintf(stdout,
                    "psxrecomp: assets: MODEL override missing "
                    "(assets/override/manifest.json); needs the mesh patcher: "
                    "python tools/assets/upgrade_model.py fix-all --root "
                    "runs/iso_full --override assets/override --no-render\n");
        }
    }

    if (out) {
        out->prepared = (st.str_status == 1) ? 1 : 0;
        out->present = (st.str_status == 2 ? 1 : 0) +
                       st.audio_present + st.hd_present + st.override_present;
        out->missing_python = (st.audio_present ? 0 : 1) +
                              (st.hd_present ? 0 : 1) +
                              (st.override_present ? 0 : 1);
    }
    rc = (st.str_status == 1) ? 1 : 0;
    return rc;
}
