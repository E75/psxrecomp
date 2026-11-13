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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CDDA_SECTOR_FRAMES 588
#define CDDA_MAX_TRACKS    16
#define WAV_NAME_MAX       160
#define CFG_PATH_MAX       512

/* ------------------------------------------------------------------ paths */

static int path_is_dir(const char* path) {
    if (!path || !*path) return 0;
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

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

static int path_is_absolute(const char* p) {
    if (!p || !*p) return 0;
    if (p[0] == '/' || p[0] == '\\') return 1;
    return isalpha((unsigned char)p[0]) && p[1] == ':';
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

/* [audio] cdda_dir = "..." — read here rather than through config_loader.h so
 * this module stays usable by the standalone unit tests. */
static int config_cdda_dir(char* out, size_t cap) {
    char cfg[CFG_PATH_MAX];
    if (!find_config(cfg, sizeof(cfg))) return 0;
    FILE* f = fopen(cfg, "rb");
    if (!f) return 0;

    char line[512];
    int in_audio = 0, got = 0;
    while (!got && fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == '\r' || *s == '\n' || !*s) continue;
        if (*s == '[') { in_audio = strncmp(s, "[audio]", 7) == 0; continue; }
        if (!in_audio) continue;
        if (strncmp(s, "cdda_dir", 8) != 0) continue;
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
    char     dir_source[24];   /* "env" | "config" | "default" */

    /* CD-DA */
    AudioNativeCddaTrack tracks[CDDA_MAX_TRACKS];
    NativeWav track_wav[CDDA_MAX_TRACKS];
    int      track_count;
    int      native_on;
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

static void resolve_dir(void) {
    char configured[CFG_PATH_MAX] = "";
    char exe[CFG_PATH_MAX], cand[CFG_PATH_MAX];
    exe_dir(exe, sizeof(exe));

    const char* env = getenv("PSX_CDDA_DIR");
    if (env && *env) {
        snprintf(configured, sizeof(configured), "%s", env);
        snprintf(g.dir_source, sizeof(g.dir_source), "env");
    } else if (config_cdda_dir(configured, sizeof(configured))) {
        snprintf(g.dir_source, sizeof(g.dir_source), "config");
    } else {
        snprintf(configured, sizeof(configured), "assets/audio");
        snprintf(g.dir_source, sizeof(g.dir_source), "default");
    }

    /* First candidate that exists as a directory wins; otherwise keep the
     * configured string so the state report still shows where to put files. */
    snprintf(g.dir, sizeof(g.dir), "%s", configured);
    if (path_is_dir(configured)) return;
    if (!path_is_absolute(configured)) {
        if (exe[0]) {
            join_path(cand, sizeof(cand), exe, configured);
            if (path_is_dir(cand)) { snprintf(g.dir, sizeof(g.dir), "%s", cand); return; }
            join_path(cand, sizeof(cand), exe, "../");
            strncat(cand, configured, sizeof(cand) - strlen(cand) - 1);
            if (path_is_dir(cand)) { snprintf(g.dir, sizeof(g.dir), "%s", cand); return; }
        }
    }
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

    int found = 0;
    char stem[WAV_NAME_MAX], path[CFG_PATH_MAX];
    for (int i = 0; i < count; ++i) {
        g.track_wav[i].ok = 0;
        iso_stem(tracks[i].iso_path, stem, sizeof(stem));
        join_path(path, sizeof(path), g.dir, "");
        snprintf(path + strlen(path), sizeof(path) - strlen(path), "%s.wav", stem);
        if (wav_open(&g.track_wav[i], path)) { found = 1; continue; }
        /* extract_xa.py's fallback name for a track with no matching entry. */
        join_path(path, sizeof(path), g.dir, "");
        snprintf(path + strlen(path), sizeof(path) - strlen(path),
                 "cdda_track%02d.wav", i + 2);
        if (wav_open(&g.track_wav[i], path)) { found = 1; }
    }

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

void audio_native_xa_configure(uint32_t str_lba, uint32_t str_sectors,
                               const char* iso_path) {
    audio_native_init();
    if (g.xa_on || !iso_path || !*iso_path) return;
    char stem[WAV_NAME_MAX], path[CFG_PATH_MAX];
    iso_stem(iso_path, stem, sizeof(stem));
    join_path(path, sizeof(path), g.dir, "");
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "%s_ch01.wav", stem);
    if (!wav_open(&g.xa, path)) return;
    g.str_lba = str_lba;
    g.str_sectors = str_sectors;
    g.xa_on = 1;
    g.xa_pos = 0;
    g.xa_edge = 0;
    snprintf(g.xastream.source, sizeof(g.xastream.source), "native");
    snprintf(g.xastream.file, sizeof(g.xastream.file), "%s", g.xa.name);
    g.xastream.rate = g.xa.rate;
    g.xastream.channels = g.xa.channels;
    printf("audio: XA from %s (%s, %d Hz)\n", path, g.xa.name, g.xa.rate);
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

/* -------------------------------------------------------------- state */

int audio_native_state_json(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    if (!g.inited) audio_native_init();
    const char* cdda_src = g.native_on ? "native"
        : (g.disc_audio_tracks > 0 ? "emulated-cd" : "none");
    const char* xa_src = g.xa_on ? "native" : "emulated-cd";
    return snprintf(out, (size_t)cap,
        "{\"cdda_dir\":\"%s\",\"cdda_dir_source\":\"%s\","
        "\"cdda\":{\"source\":\"%s\",\"file\":\"%s\",\"rate\":%d,"
        "\"channels\":%d,\"frames\":%llu,\"underruns\":%llu,\"track\":%d,"
        "\"tracks\":%d,\"disc_audio_tracks\":%d},"
        "\"xa\":{\"source\":\"%s\",\"file\":\"%s\",\"rate\":%d,"
        "\"out_rate\":44100,\"channels\":%d,\"frames\":%llu,"
        "\"underruns\":%llu,\"active\":%d,\"str_lba\":%u}}",
        g.dir, g.dir_source,
        cdda_src, g.native_on ? g.cdda.file : "",
        g.native_on ? 44100 : 0, g.native_on ? 2 : 0,
        (unsigned long long)g.cdda.frames,
        (unsigned long long)g.cdda.underruns,
        g.last_track, g.native_on ? g.track_count + 1 : 0,
        g.disc_audio_tracks,
        xa_src, g.xa_on ? g.xastream.file : "",
        g.xa_on ? g.xa.rate : 0, g.xa_on ? g.xastream.channels : 0,
        (unsigned long long)g.xastream.frames,
        (unsigned long long)g.xastream.underruns,
        g.xa_edge, g.str_lba);
}

void audio_native_state_flush(void) {
    if (!g.inited) return;
    char json[1024], path[CFG_PATH_MAX], exe[CFG_PATH_MAX];
    audio_native_state_json(json, (int)sizeof(json));
    exe_dir(exe, sizeof(exe));
    join_path(path, sizeof(path), exe, "audio_state.json");
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fputs(json, f);
    fputc('\n', f);
    fclose(f);
}
