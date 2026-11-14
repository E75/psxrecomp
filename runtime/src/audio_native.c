/*
 * audio_native.c — see audio_native.h.
 *
 * Two file-backed sources, no dependency on the CD model or the SPU:
 *
 *   CD-DA: the eight `SD/DA/<name>.DA` entries of the retail disc are its Red Book
 *          music tracks, which a Track-1-only dump does not contain. When the
 *          mounted image has no audio tracks of its own, the track table is
 *          built from the ISO9660 entries (start LBA = entry LBA, length =
 *          size/2048 sectors, 150-sector pre-gap between entries) and every
 *          sector read is served from `assets/audio/<STEM>.wav` — 588
 *          interleaved stereo frames per sector, exactly the CD controller's
 *          unit, so cdrom.c keeps applying the volume matrix, muting,
 *          position reports and the track-end INT4.
 *
 *   XA: `MOVIE/WE2002.STR` interleaves one stereo 4-bit 37.8 kHz XA channel
 *      7 video sectors : 1 audio sector. The decoded WAV is substituted at
 *       the SPU CD-input seam, driven by the emulated stream's own push
 *       cadence (one push == one XA sector's worth of 44.1 kHz frames), so
 *       the audio stays locked to the video without a second clock.
 *
 * Nothing here touches the guest; all failures fall back to the emulated CD.
 */

#include "audio_native.h"
#include "host_path_resolve.h"   /* docs/ASSET_PATHS.md: the shared path rule */
#include "resolved_paths.h"      /* record both keys for `resolved_paths` */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CDDA_SECTOR_FRAMES 588
#define CDDA_MAX_TRACKS    16
#define WAV_NAME_MAX       160
#define CFG_PATH_MAX       512

/* ------------------------------------------------------------------ paths */

/* Directory holding the executable (empty when it cannot be determined). */
static void exe_dir(char* out, size_t cap) {
    out[0] = '\0';
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) { out[0] = '\0'; return; }
    for (DWORD i = n; i > 0; --i) {
        if (out[i - 1] == '\\' || out[i - 1] == '/') { out[i - 1] = '\0'; return; }
    }
    out[0] = '\0';
#else
    char buf[CFG_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    char* slash = strrchr(buf, '/');
    if (!slash) return;
    *slash = '\0';
    snprintf(out, cap, "%s", buf);
#endif
}

static void join_path(char* out, size_t cap, const char* dir, const char* rel) {
    if (!dir || !*dir) { snprintf(out, cap, "%s", rel); return; }
    size_t n = strlen(dir);
    const char* sep = (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/";
    snprintf(out, cap, "%s%s%s", dir, sep, rel);
}

/* --------------------------------------------------------- config parsing */

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* First existing game.toml: $PSX_GAME_TOML, cwd, exe dir, exe dir/.. */
static int find_config(char* out, size_t cap) {
    const char* env = getenv("PSX_GAME_TOML");
    char exe[CFG_PATH_MAX], cand[CFG_PATH_MAX];
    exe_dir(exe, sizeof(exe));

    if (env && *env && file_exists(env)) return snprintf(out, cap, "%s", env) > 0;
    if (file_exists("game.toml")) { snprintf(out, cap, "game.toml"); return 1; }
    if (exe[0]) {
        join_path(cand, sizeof(cand), exe, "game.toml");
        if (file_exists(cand)) { snprintf(out, cap, "%s", cand); return 1; }
        join_path(cand, sizeof(cand), exe, "../game.toml");
        if (file_exists(cand)) { snprintf(out, cap, "%s", cand); return 1; }
    }
    return 0;
}

/* One string key from the [audio] block — read here rather than through
 * config_loader.h so this module stays usable by the standalone unit tests. */
static int config_audio_key(const char* key, char* out, size_t cap) {
    char cfg[CFG_PATH_MAX];
    if (!find_config(cfg, sizeof(cfg))) return 0;
    FILE* f = fopen(cfg, "rb");
    if (!f) return 0;

    const size_t key_len = strlen(key);
    char line[512];
    int in_audio = 0, got = 0;
    while (!got && fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == '\r' || *s == '\n' || !*s) continue;
        if (*s == '[') { in_audio = strncmp(s, "[audio]", 7) == 0; continue; }
        if (!in_audio) continue;
        if (strncmp(s, key, key_len) != 0) continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        size_t len;
        if (*v == '"' || *v == '\'') {
            char quote = *v++;
            char* end = strchr(v, quote);
            if (!end) continue;
            len = (size_t)(end - v);
        } else {
            len = strcspn(v, " \t\r\n#");
        }
        if (len == 0 || len >= cap) continue;
        memcpy(out, v, len);
        out[len] = '\0';
        got = 1;
    }
    fclose(f);
    return got;
}

/* --------------------------------------------------------------- WAV file */

typedef struct NativeWav {
    FILE*    f;
    uint32_t data_off;
    uint64_t data_bytes;
    int      rate;
    int      channels;
    int      bits;
    char     name[WAV_NAME_MAX];
    int      ok;
} NativeWav;

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wav_close(NativeWav* w) {
    if (w->f) fclose(w->f);
    memset(w, 0, sizeof(*w));
}

static int wav_open(NativeWav* w, const char* path) {
    memset(w, 0, sizeof(*w));
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return 0;
    }

    int have_fmt = 0, have_data = 0;
    uint8_t chunk[8];
    while (fread(chunk, 1, 8, f) == 8) {
        uint32_t size = rd32(chunk + 4);
        long here = ftell(f);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < 16 || fread(fmt, 1, 16, f) != 16) { fclose(f); return 0; }
            if (rd16(fmt + 0) != 1) { fclose(f); return 0; }   /* PCM only */
            w->channels = (int)rd16(fmt + 2);
            w->rate     = (int)rd32(fmt + 4);
            w->bits     = (int)rd16(fmt + 14);
            have_fmt = 1;
        } else if (memcmp(chunk, "data", 4) == 0) {
            w->data_off = (uint32_t)here;
            w->data_bytes = size;
            have_data = 1;
        }
        if (have_fmt && have_data) break;
        if (fseek(f, here + (long)size + (long)(size & 1u), SEEK_SET) != 0) break;
    }

    if (!have_fmt || !have_data || w->bits != 16 ||
        (w->channels != 1 && w->channels != 2) || w->rate <= 0) {
        fclose(f);
        return 0;
    }
    w->f = f;
    w->ok = 1;
    const char* base = path;
    for (const char* p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(w->name, sizeof(w->name), "%s", base);
    return 1;
}

/* Read `frames` stereo frames starting at `frame`; mono is upmixed. Returns
 * the number of frames actually produced (short at EOF). */
static int wav_read_frames(NativeWav* w, uint64_t frame, int frames,
                           int16_t* out) {
    if (!w->ok || !w->f || frames <= 0) return 0;
    uint64_t frame_bytes = (uint64_t)w->channels * 2u;
    uint64_t off = (uint64_t)w->data_off + frame * frame_bytes;
    if (off + (uint64_t)frames * frame_bytes > (uint64_t)w->data_off + w->data_bytes)
        return 0;
    if (fseek(w->f, (long)off, SEEK_SET) != 0) return 0;
    if (w->channels == 2) {
        size_t want = (size_t)frames * 4u;
        if (fread(out, 1, want, w->f) != want) return 0;
        return frames;
    }
    /* mono: read into the tail of the destination and expand backwards. */
    int16_t* mono = out + frames;   /* non-overlapping scratch region */
    size_t want = (size_t)frames * 2u;
    if (fread(mono, 1, want, w->f) != want) return 0;
    for (int i = frames - 1; i >= 0; --i) {
        int16_t s = mono[i];
        out[i * 2 + 0] = s;
        out[i * 2 + 1] = s;
    }
    return frames;
}

/* --------------------------------------------------------------- state */

typedef struct AudioStreamState {
    char     source[24];    /* "native" | "emulated-cd" | "none" */
    char     file[WAV_NAME_MAX];
    int      rate;
    int      channels;
    uint64_t frames;
    uint64_t underruns;
} AudioStreamState;

static struct {
    int      inited;
    char     dir[CFG_PATH_MAX];
    char     dir_cfg[CFG_PATH_MAX];  /* configured value, as written */
    char     dir_source[24];   /* "env" | "config" | "default" */
    char     str_dir[CFG_PATH_MAX];
    char     str_cfg[CFG_PATH_MAX];
    int      logged;

    /* CD-DA */
    AudioNativeCddaTrack tracks[CDDA_MAX_TRACKS];
    NativeWav track_wav[CDDA_MAX_TRACKS];
    int      track_count;
    int      native_on;
    int      cdda_decoded;     /* track WAVs opened by the last configure */
    int      disc_audio_tracks;
    int      last_track;       /* 1-based disc track number, 0 = none */

    /* XA */
    NativeWav xa;
    uint32_t  str_lba;
    uint32_t  str_sectors;
    int       xa_on;
    int       xa_edge;
    uint64_t  xa_pos;          /* native frames consumed */
    uint64_t  xa_underruns;

    /* Raw STR sectors */
    FILE*     raw_f;
    uint32_t  raw_data_off;
    uint32_t  raw_start_lba;
    uint32_t  raw_sectors;
    int       raw_tried;       /* open attempted */
    int       raw_on;
    uint64_t  raw_served;
    char      raw_name[WAV_NAME_MAX];

    AudioStreamState cdda;
    AudioStreamState xastream;
} g;

/* ---------------------------------------------------------------- helpers */

/* extract_xa.py's stem rule: upper-case, non-alphanumeric runs -> '_'. */
static void iso_stem(const char* iso_path, char* out, size_t cap) {
    size_t o = 0;
    int last_us = 1;
    for (const char* p = iso_path; *p && o + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            out[o++] = (char)toupper(c);
            last_us = 0;
        } else if (!last_us) {
            out[o++] = '_';
            last_us = 1;
        }
    }
    while (o > 0 && out[o - 1] == '_') --o;
    out[o] = '\0';
}

/* The four anchors of the path rule (docs/ASSET_PATHS.md). main.cpp installs
 * the game.toml project dir + exe dir at startup; a module used on its own
 * (unit tests) falls back to the exe dir on first resolve. */
static PsxPathRoots g_roots;
static int          g_roots_ready;
static int          g_roots_authoritative;   /* installed by main.cpp */

static void resolve_dir(void);

void audio_native_set_path_roots(const char* config_dir, const char* exe) {
    psx_path_roots_init(&g_roots, config_dir, exe);
    g_roots_ready = 1;
    g_roots_authoritative = 1;
    if (g.inited) {
        g.logged = 0;
        resolve_dir();   /* roots arrived after a lazy init */
    }
}

static void ensure_roots(void) {
    char exe[CFG_PATH_MAX];
    if (g_roots_ready) return;
    exe_dir(exe, sizeof exe);
    psx_path_roots_init(&g_roots, NULL, exe);
    g_roots_ready = 1;
}

/* Directory part of a path, without the trailing separator. */
static void dir_of(const char* path, char* out, size_t cap) {
    size_t n = path ? strlen(path) : 0;
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\') --n;
    if (n > 0) --n;
    if (n == 0) { snprintf(out, cap, "."); return; }
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

/* First anchor that holds `rel` as a real file. A directory that merely exists
 * is NOT a candidate: an empty build-tree `assets/audio` must never shadow the
 * checkout copy that actually holds the WAV, because docs/ASSET_PATHS.md
 * resolves a key to an artefact. Order: game.toml project dir, exe dir, its
 * parent, cwd (the shared rule in host_path_resolve.h). */
static int find_audio_path(const char* rel, char* out, size_t cap) {
    char candidate[PSX_PATH_RESOLVE_MAX];
    const char* anchors[4];
    int i;
    if (!rel || !rel[0]) return 0;
    ensure_roots();
    if (psx_path_is_absolute(rel)) {
        snprintf(candidate, sizeof candidate, "%s", rel);
        psx_path_normalize(candidate);
        if (psx_path_exists(candidate)) { snprintf(out, cap, "%s", candidate); return 1; }
        return 0;
    }
    anchors[0] = g_roots.config_dir;
    anchors[1] = g_roots.exe_dir;
    anchors[2] = g_roots.exe_parent;
    anchors[3] = g_roots.cwd;
    for (i = 0; i < 4; ++i) {
        if (!anchors[i][0] || (anchors[i][0] == '.' && anchors[i][1] == '\0'))
            continue;   /* psx_path_normalize("") spells an empty root "." */
        psx_path_join(candidate, sizeof candidate, anchors[i], rel);
        psx_path_normalize(candidate);
        if (psx_path_exists(candidate)) { snprintf(out, cap, "%s", candidate); return 1; }
    }
    return 0;
}

/* `<configured>/<name>` as a real file. `name` may be NULL for a file-valued
 * key (an explicit `.raw` path). */
static int find_audio_file(const char* configured, const char* name,
                           char* out, size_t cap) {
    char rel[PSX_PATH_RESOLVE_MAX];
    if (!configured || !configured[0]) return 0;
    if (name && name[0]) psx_path_join(rel, sizeof rel, configured, name);
    else snprintf(rel, sizeof rel, "%s", configured);
    return find_audio_path(rel, out, cap);
}

/* *.wav files in a directory, 0 when it is absent. Reported by `audio_state`
 * so "the WAVs exist but nothing is native" is answerable in one query. */
static int dir_count_wavs(const char* dir) {
    int n = 0;
    if (!dir || !dir[0]) return 0;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    char pattern[CFG_PATH_MAX];
    snprintf(pattern, sizeof pattern, "%s\\*.wav", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    struct dirent* e;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l > 4 && strcasecmp(e->d_name + l - 4, ".wav") == 0) ++n;
    }
    closedir(d);
#endif
    return n;
}

/* First existing candidate for a configured path, docs/ASSET_PATHS.md: the
 * game.toml project dir, the exe dir, its parent, then the cwd. This is the
 * directory-level answer used for the report fallback; the file-level lookup
 * above decides what is actually opened. */
static void resolve_key(const char* configured, const char* source,
                        char* out, size_t cap) {
    ensure_roots();
    psx_path_resolve(&g_roots, configured, out, cap);
    (void)source;
}

static void xa_probe(void);
static void raw_retry(void);
static void raw_open(void);
static void audio_native_log_sources(void);

static void resolve_dir(void) {
    char configured[CFG_PATH_MAX] = "";

    const char* env = getenv("PSX_CDDA_DIR");
    if (env && *env) {
        snprintf(configured, sizeof(configured), "%s", env);
        snprintf(g.dir_source, sizeof(g.dir_source), "env");
    } else if (config_audio_key("cdda_dir", configured, sizeof(configured))) {
        snprintf(g.dir_source, sizeof(g.dir_source), "config");
    } else {
        snprintf(configured, sizeof(configured), "assets/audio");
        snprintf(g.dir_source, sizeof(g.dir_source), "default");
    }
    snprintf(g.dir_cfg, sizeof(g.dir_cfg), "%s", configured);
    resolve_key(configured, g.dir_source, g.dir, sizeof(g.dir));

    if (!config_audio_key("str_raw", configured, sizeof(configured)))
        snprintf(configured, sizeof(configured), "assets/str");
    snprintf(g.str_cfg, sizeof(g.str_cfg), "%s", configured);
    resolve_key(configured, "config", g.str_dir, sizeof(g.str_dir));

    /* Load what the files themselves say: the movie WAV and the raw extent are
     * known by name, so availability is decided here instead of waiting for a
     * CD read. Each successful open re-points the reported dir at the copy that
     * actually holds the file. */
    xa_probe();
    raw_retry();

    if (g_roots_authoritative) {
        psx_resolved_paths_note("audio.cdda_dir", g.dir_cfg, g.dir, g.dir_source);
        psx_resolved_paths_note("audio.str_raw", g.str_cfg, g.str_dir, "config");
    }
    audio_native_log_sources();
}

void audio_native_init(void) {
    if (g.inited) return;
    g.inited = 1;
    memset(g.cdda.source, 0, sizeof(g.cdda.source));
    memset(g.xastream.source, 0, sizeof(g.xastream.source));
    snprintf(g.cdda.source, sizeof(g.cdda.source), "none");
    snprintf(g.xastream.source, sizeof(g.xastream.source), "none");
    resolve_dir();
}

/* --------------------------------------------------------------- CD-DA */

void audio_native_cdda_note_disc_audio(int audio_tracks) {
    audio_native_init();
    g.disc_audio_tracks = audio_tracks;
    if (audio_tracks > 0 && g.native_on) {
        /* The mounted image carries real audio: prefer the disc. */
        for (int i = 0; i < g.track_count; ++i) wav_close(&g.track_wav[i]);
        g.track_count = 0;
        g.native_on = 0;
        snprintf(g.cdda.source, sizeof(g.cdda.source), "emulated-cd");
    }
}

void audio_native_cdda_configure(const AudioNativeCddaTrack* tracks, int count) {
    audio_native_init();
    if (g.native_on || !tracks || count <= 0) return;
    if (count > CDDA_MAX_TRACKS) count = CDDA_MAX_TRACKS;
    if (g.disc_audio_tracks > 0) return;   /* the disc is authoritative */

    memcpy(g.tracks, tracks, (size_t)count * sizeof(tracks[0]));
    g.track_count = count;

    int found = 0, decoded = 0;
    char stem[WAV_NAME_MAX], name[WAV_NAME_MAX], path[CFG_PATH_MAX];
    for (int i = 0; i < count; ++i) {
        g.track_wav[i].ok = 0;
        iso_stem(tracks[i].iso_path, stem, sizeof(stem));
        path[0] = '\0';
        snprintf(name, sizeof(name), "%s.wav", stem);
        if (!find_audio_file(g.dir_cfg, name, path, sizeof(path))) {
            /* extract_xa.py's fallback name for a track with no entry. */
            snprintf(name, sizeof(name), "cdda_track%02d.wav", i + 2);
            if (!find_audio_file(g.dir_cfg, name, path, sizeof(path)))
                path[0] = '\0';
        }
        if (path[0] && wav_open(&g.track_wav[i], path)) {
            if (!found) dir_of(path, g.dir, sizeof(g.dir));
            found = 1;
            decoded++;
        }
    }
    g.cdda_decoded = decoded;

    if (!found) {
        /* No extracted audio: leave the CD model exactly as it was. */
        g.track_count = 0;
        return;
    }
    g.native_on = 1;
    snprintf(g.cdda.source, sizeof(g.cdda.source), "native");
    snprintf(g.cdda.file, sizeof(g.cdda.file), "%s", g.track_wav[0].name);
    g.cdda.rate = 44100;
    g.cdda.channels = 2;
    printf("audio: CD-DA from %s (%d tracks, %s)\n", g.dir, count, g.dir_source);
}

int audio_native_cdda_track_count(void) { return g.native_on ? g.track_count + 1 : 0; }

uint32_t audio_native_cdda_track_start(int track) {
    if (!g.native_on) return 0;
    if (track <= 0) return audio_native_cdda_sector_count();
    if (track == 1) return 0;
    int i = track - 2;
    return (i >= 0 && i < g.track_count) ? g.tracks[i].start_lba : 0;
}

uint32_t audio_native_cdda_track_pregap(int track) {
    if (!g.native_on) return 0;
    if (track <= 1) return 0;
    int i = track - 2;
    if (i < 0 || i >= g.track_count) return 0;
    uint32_t start = g.tracks[i].start_lba;
    /* Every SD/DA entry is preceded by the next track's 150-sector pre-gap;
     * clamp so the gap can never swallow the previous track's tail. */
    uint32_t prev_end = (i > 0) ? g.tracks[i - 1].start_lba + g.tracks[i - 1].sectors : 0;
    uint32_t gap = (start > 150u) ? start - 150u : 0u;
    return (gap > prev_end) ? gap : prev_end;
}

uint32_t audio_native_cdda_sector_count(void) {
    if (!g.native_on || g.track_count <= 0) return 0;
    const AudioNativeCddaTrack* last = &g.tracks[g.track_count - 1];
    return last->start_lba + last->sectors;
}

int audio_native_cdda_track_for_lba(uint32_t lba) {
    if (!g.native_on) return 0;
    int found = 0;
    for (int i = 0; i < g.track_count; ++i) {
        if (audio_native_cdda_track_pregap(i + 2) > lba) break;
        found = i + 2;
    }
    return found;
}

int audio_native_cdda_read_sector(uint32_t lba, uint8_t* out, int size) {
    if (!g.native_on || !out || size < CDDA_SECTOR_FRAMES * 4) return 0;
    for (int i = 0; i < g.track_count; ++i) {
        const AudioNativeCddaTrack* t = &g.tracks[i];
        if (lba < t->start_lba || lba - t->start_lba >= t->sectors) continue;
        uint64_t frame = (uint64_t)(lba - t->start_lba) * CDDA_SECTOR_FRAMES;
        int16_t pcm[CDDA_SECTOR_FRAMES * 2];
        if (wav_read_frames(&g.track_wav[i], frame, CDDA_SECTOR_FRAMES, pcm) !=
            CDDA_SECTOR_FRAMES) {
            g.cdda.underruns++;
            return 0;
        }
        memcpy(out, pcm, (size_t)CDDA_SECTOR_FRAMES * 4u);
        g.cdda.frames += CDDA_SECTOR_FRAMES;
        snprintf(g.cdda.file, sizeof(g.cdda.file), "%s", g.track_wav[i].name);
        if (g.last_track != i + 2) {
            g.last_track = i + 2;
            audio_native_state_flush();
        }
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- XA */

/* Record an opened XA WAV as the live source. */
static void xa_activate(const char* path) {
    g.xa_on = 1;
    g.xa_pos = 0;
    g.xa_edge = 0;
    dir_of(path, g.dir, sizeof(g.dir));
    snprintf(g.xastream.source, sizeof(g.xastream.source), "native");
    snprintf(g.xastream.file, sizeof(g.xastream.file), "%s", g.xa.name);
    g.xastream.rate = g.xa.rate;
    g.xastream.channels = g.xa.channels;
    printf("audio: XA from %s (%s, %d Hz)\n", path, g.xa.name, g.xa.rate);
}

/* Availability at startup, before any CD read: the movie's decoded WAV is a
 * known file name, so `audio_state` can say "native" (loaded, will be used)
 * while `xa.active` still says whether it is playing. */
static void xa_probe(void) {
    char path[CFG_PATH_MAX];
    if (g.xa_on || !g.dir_cfg[0]) return;
    if (!find_audio_file(g.dir_cfg, "MOVIE_WE2002_STR_ch01.wav", path, sizeof(path)))
        return;
    if (!wav_open(&g.xa, path)) return;
    xa_activate(path);
}

void audio_native_xa_configure(uint32_t str_lba, uint32_t str_sectors,
                               const char* iso_path) {
    audio_native_init();
    if (!iso_path || !*iso_path) return;
    if (g.xa_on) {
        /* Already loaded by the startup probe; the ISO walk only supplies the
         * extent the substitution guard needs. */
        g.str_lba = str_lba;
        g.str_sectors = str_sectors;
        return;
    }
    char stem[WAV_NAME_MAX], name[WAV_NAME_MAX], path[CFG_PATH_MAX];
    iso_stem(iso_path, stem, sizeof(stem));
    snprintf(name, sizeof(name), "%s_ch01.wav", stem);
    if (!find_audio_file(g.dir_cfg, name, path, sizeof(path))) return;
    if (!wav_open(&g.xa, path)) return;
    g.str_lba = str_lba;
    g.str_sectors = str_sectors;
    xa_activate(path);
}

/* Same linear resample as cdrom.c's xa_resample_to_44100 so the native stream
 * lands on identical 44.1 kHz frames (37800 -> 44100 is an exact 2352/2016). */
static int resample_to_44100(const int16_t* in, int in_frames, int in_rate,
                             int16_t* out, int out_frames) {
    int produced = 0, pos = 0, phase = 0;
    while (produced < out_frames && pos < in_frames) {
        int next = (pos + 1 < in_frames) ? pos + 1 : pos;
        int32_t cur_l = in[pos * 2 + 0], cur_r = in[pos * 2 + 1];
        int32_t nxt_l = in[next * 2 + 0], nxt_r = in[next * 2 + 1];
        out[produced * 2 + 0] = (int16_t)(cur_l + ((nxt_l - cur_l) * phase) / 44100);
        out[produced * 2 + 1] = (int16_t)(cur_r + ((nxt_r - cur_r) * phase) / 44100);
        produced++;
        phase += in_rate;
        while (phase >= 44100) { phase -= 44100; pos++; }
    }
    return produced;
}

int audio_native_xa_fill(const int16_t* emulated, int frames, int16_t* out,
                         int stream_active, int delivered_lba) {
    if (!emulated || !out || frames <= 0 || frames > AUDIO_NATIVE_XA_MAX_FRAMES)
        return 0;
    if (!g.inited) audio_native_init();
    if (!g.xa_on) return 0;

    if (!stream_active) {
        if (g.xa_edge) { g.xa_edge = 0; audio_native_state_flush(); }
        return 0;
    }

    if (!g.xa_edge) {
        /* Stream start: only substitute when the drive is reading inside the
         * STR extent this WAV was decoded from, so an unrelated XA stream
         * keeps the emulated path. */
        if (g.str_sectors &&
            (delivered_lba < (int)g.str_lba ||
             delivered_lba > (int)(g.str_lba + g.str_sectors)))
            return 0;
        g.xa_edge = 1;
        g.xa_pos = 0;
        audio_native_state_flush();
    }

    /* An all-zero push is a muted drive (FMV auto-skip) or a silent decode:
     * keep the emulated samples so muting still works. */
    int nonzero = 0;
    for (int i = 0; i < frames * 2; ++i) {
        if (emulated[i]) { nonzero = 1; break; }
    }
    if (!nonzero) return 0;

    int in_frames = (int)(((int64_t)frames * g.xa.rate) / 44100);
    if (in_frames < 2) return 0;
    int16_t native[AUDIO_NATIVE_XA_MAX_FRAMES * 2];
    if (wav_read_frames(&g.xa, g.xa_pos, in_frames, native) != in_frames) {
        g.xa_underruns++;
        g.xastream.underruns = g.xa_underruns;
        return 0;
    }
    int got = resample_to_44100(native, in_frames, g.xa.rate, out, frames);
    for (int i = got; i < frames; ++i) {
        out[i * 2 + 0] = 0;
        out[i * 2 + 1] = 0;
    }
    g.xa_pos += (uint64_t)in_frames;
    g.xastream.frames += (uint64_t)frames;
    return 1;
}

/* ------------------------------------------------------------- raw STR */

#define STR_RAW_MAGIC   "PSTR"
#define STR_RAW_VERSION 1u
#define STR_RAW_HEADER  32u
#define STR_RAW_SECTOR  2352u

/* Open the raw extent once. The header carries the LBA range, so the file also
 * works when no image is mounted to walk the ISO9660 tree. The configured value
 * is a directory (`<dir>/WE2002.STR.raw`) or an explicit `.raw` file, and the
 * file itself decides which anchor wins — an existing but empty directory is
 * never enough. */
static void raw_open(void) {
    char path[CFG_PATH_MAX];
    if (g.raw_tried) return;
    g.raw_tried = 1;

    size_t n = strlen(g.str_cfg);
    const int file_key = (n > 4 && strcmp(g.str_cfg + n - 4, ".raw") == 0);
    if (!find_audio_file(g.str_cfg, file_key ? NULL : "WE2002.STR.raw",
                         path, sizeof(path)))
        return;

    FILE* f = fopen(path, "rb");
    if (!f) return;
    uint8_t hdr[STR_RAW_HEADER];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, STR_RAW_MAGIC, 4) != 0) {
        fclose(f);
        return;
    }
    const uint32_t version = rd32(hdr + 4);
    const uint32_t start   = rd32(hdr + 8);
    const uint32_t sectors = rd32(hdr + 12);
    const uint32_t ssize   = rd32(hdr + 16);
    if (version != STR_RAW_VERSION || ssize != STR_RAW_SECTOR || sectors == 0) {
        fclose(f);
        return;
    }
    g.raw_f = f;
    g.raw_data_off = STR_RAW_HEADER;
    g.raw_start_lba = start;
    g.raw_sectors = sectors;
    g.raw_on = 1;
    dir_of(path, g.str_dir, sizeof(g.str_dir));
    const char* base = path;
    for (const char* p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(g.raw_name, sizeof(g.raw_name), "%s", base);
    printf("audio: STR raw sectors from %s (%u sectors at LBA %u)\n",
           path, (unsigned)sectors, (unsigned)start);
}

/* Re-attempt the open after the path roots arrive (a first attempt may have
 * looked in the wrong place). Keeps an already-open extent. */
static void raw_retry(void) {
    if (g.raw_on) return;
    g.raw_tried = 0;
    raw_open();
}

/* One startup line: the roots and what each stream will use. Everything is
 * greppable under `audio:` — the acceptance run answers "why is nothing
 * native" with `findstr audio:` on the log. */
static void audio_native_log_sources(void) {
    if (g.logged) return;
    g.logged = 1;
    printf("audio: roots cdda_dir=%s (wavs=%d, %s) str_raw=%s | "
           "cdda=%s xa=%s str_raw=%s | files cdda=%s xa=%s raw=%s\n",
           g.dir, dir_count_wavs(g.dir), g.dir_source, g.str_dir,
           g.native_on ? "native" : (g.disc_audio_tracks > 0 ? "emulated-cd" : "none"),
           g.xa_on ? "native" : "emulated-cd",
           g.raw_on ? "native" : "disc",
           g.cdda.file[0] ? g.cdda.file : "-",
           g.xa_on ? g.xastream.file : "-",
           g.raw_on ? g.raw_name : "-");
    fflush(stdout);
}

void audio_native_str_raw_configure(uint32_t start_lba, uint32_t sectors) {
    audio_native_init();
    raw_open();
    if (g.raw_on) return;   /* the file's own header is authoritative */
    g.raw_start_lba = start_lba;
    g.raw_sectors = sectors;
}

int audio_native_str_raw_read(uint32_t lba, uint8_t* out, int size) {
    if (!out || size < (int)STR_RAW_SECTOR) return 0;
    if (!g.inited) audio_native_init();
    if (!g.raw_tried) raw_open();
    if (!g.raw_on) return 0;
    if (lba < g.raw_start_lba || lba - g.raw_start_lba >= g.raw_sectors) return 0;
    const long off = (long)g.raw_data_off +
        (long)((uint64_t)(lba - g.raw_start_lba) * STR_RAW_SECTOR);
    if (fseek(g.raw_f, off, SEEK_SET) != 0) return 0;
    if (fread(out, 1, STR_RAW_SECTOR, g.raw_f) != STR_RAW_SECTOR) return 0;
    g.raw_served++;
    return 1;
}

/* -------------------------------------------------------------- state */

int audio_native_state_json(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    if (!g.inited) audio_native_init();
    const char* cdda_src = g.native_on ? "native"
        : (g.disc_audio_tracks > 0 ? "emulated-cd" : "none");
    const char* xa_src = g.xa_on ? "native" : "emulated-cd";
    /* `source` = availability (the file is loaded and will be used), `active` =
     * playing right now, `wavs` = decoded WAVs on disk in the reported dir. */
    return snprintf(out, (size_t)cap,
        "{\"cdda_dir\":\"%s\",\"cdda_dir_source\":\"%s\","
        "\"cdda\":{\"source\":\"%s\",\"available\":%d,\"decoded\":%d,"
        "\"file\":\"%s\",\"rate\":%d,"
        "\"channels\":%d,\"frames\":%llu,\"underruns\":%llu,\"track\":%d,"
        "\"tracks\":%d,\"disc_audio_tracks\":%d},"
        "\"xa\":{\"source\":\"%s\",\"available\":%d,"
        "\"file\":\"%s\",\"rate\":%d,"
        "\"out_rate\":44100,\"channels\":%d,\"frames\":%llu,"
        "\"underruns\":%llu,\"active\":%d,\"str_lba\":%u},"
        "\"str_raw\":{\"source\":\"%s\",\"available\":%d,"
        "\"file\":\"%s\",\"dir\":\"%s\","
        "\"start_lba\":%u,\"sectors\":%u,\"served\":%llu}}",
        g.dir, g.dir_source,
        cdda_src, g.native_on ? 1 : 0, g.cdda_decoded,
        g.native_on ? g.cdda.file : "",
        g.native_on ? 44100 : 0, g.native_on ? 2 : 0,
        (unsigned long long)g.cdda.frames,
        (unsigned long long)g.cdda.underruns,
        g.last_track, g.native_on ? g.track_count + 1 : 0,
        g.disc_audio_tracks,
        xa_src, g.xa_on ? 1 : 0,
        g.xa_on ? g.xastream.file : "",
        g.xa_on ? g.xa.rate : 0, g.xa_on ? g.xastream.channels : 0,
        (unsigned long long)g.xastream.frames,
        (unsigned long long)g.xastream.underruns,
        g.xa_edge, g.str_lba,
        g.raw_on ? "native" : "disc", g.raw_on ? 1 : 0,
        g.raw_on ? g.raw_name : "",
        g.str_dir, g.raw_start_lba, g.raw_sectors,
        (unsigned long long)g.raw_served);
}

void audio_native_state_flush(void) {
    if (!g.inited) return;
    char json[1536], path[CFG_PATH_MAX], exe[CFG_PATH_MAX];
    audio_native_state_json(json, (int)sizeof(json));
    exe_dir(exe, sizeof(exe));
    join_path(path, sizeof(path), exe, "audio_state.json");
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fputs(json, f);
    fputc('\n', f);
    fclose(f);
}
