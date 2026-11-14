/* vram_ext.c - host-side VRAM extension (see vram_ext.h, docs/VRAM_EXTENSION.md).
 *
 * State only: config flag, the framebuffer rect currently being redirected, the
 * host extension plane, and counters. The remap itself is per-pixel and lives
 * in the backends (Vulkan pack compute, GL pack fragment, CPU readout), so this
 * module never touches a draw. */

#include "vram_ext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define CFG_PATH_MAX 1024

/* ------------------------------------------------------------- config ---- */

static int s_inited;
static int s_enabled;
static int s_backend = VRAM_EXT_BACKEND_NONE;

static int cfg_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void cfg_exe_dir(char *out, size_t cap) {
    out[0] = '\0';
#ifdef _WIN32
    char buf[CFG_PATH_MAX];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof buf);
    if (n == 0 || n >= sizeof buf) return;
    buf[n] = '\0';
    char *slash = strrchr(buf, '\\');
    if (!slash) slash = strrchr(buf, '/');
    if (!slash) return;
    *slash = '\0';
    snprintf(out, cap, "%s", buf);
#else
    char buf[CFG_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    char *slash = strrchr(buf, '/');
    if (!slash) return;
    *slash = '\0';
    snprintf(out, cap, "%s", buf);
#endif
}

static void cfg_join(char *out, size_t cap, const char *dir, const char *rel) {
    if (!dir || !dir[0]) { snprintf(out, cap, "%s", rel); return; }
    size_t n = strlen(dir);
    const char *sep = (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/";
    snprintf(out, cap, "%s%s%s", dir, sep, rel);
}

/* First existing game.toml: $PSX_GAME_TOML, cwd, exe dir, exe dir/.. */
static int cfg_find(char *out, size_t cap) {
    const char *env = getenv("PSX_GAME_TOML");
    char exe[CFG_PATH_MAX], cand[CFG_PATH_MAX];
    cfg_exe_dir(exe, sizeof exe);

    if (env && *env && cfg_file_exists(env)) return snprintf(out, cap, "%s", env) > 0;
    if (cfg_file_exists("game.toml")) { snprintf(out, cap, "%s", "game.toml"); return 1; }
    if (exe[0]) {
        cfg_join(cand, sizeof cand, exe, "game.toml");
        if (cfg_file_exists(cand)) { snprintf(out, cap, "%s", cand); return 1; }
        cfg_join(cand, sizeof cand, exe, "../game.toml");
        if (cfg_file_exists(cand)) { snprintf(out, cap, "%s", cand); return 1; }
    }
    return 0;
}

/* One value from the [video] block. Read here rather than through
 * config_loader.h so this module stays a plain C runtime unit. */
static int cfg_video_bool(const char *key, int *out) {
    char cfg[CFG_PATH_MAX];
    if (!cfg_find(cfg, sizeof cfg)) return 0;
    FILE *f = fopen(cfg, "rb");
    if (!f) return 0;

    const size_t key_len = strlen(key);
    char line[512];
    int in_video = 0, got = 0;
    while (!got && fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == '\r' || *s == '\n' || !*s) continue;
        if (*s == '[') { in_video = strncmp(s, "[video]", 7) == 0; continue; }
        if (!in_video) continue;
        if (strncmp(s, key, key_len) != 0) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        if (*v == '"' || *v == '\'') {
            char quote = *v++;
            char *end = strchr(v, quote);
            if (!end) continue;
            size_t len = (size_t)(end - v);
            if (len >= 64) continue;
            char tmp[64];
            memcpy(tmp, v, len);
            tmp[len] = '\0';
            *out = (strcmp(tmp, "true") == 0 || strcmp(tmp, "1") == 0 ||
                    strcmp(tmp, "yes") == 0 || strcmp(tmp, "on") == 0);
        } else {
            *out = (*v == 't' || *v == 'T' || *v == '1' || *v == 'y' || *v == 'Y');
        }
        got = 1;
    }
    fclose(f);
    return got;
}

static int s_sw_warned;

static void warn_if_software(void) {
    if (!s_enabled || s_sw_warned || s_backend != VRAM_EXT_BACKEND_SOFTWARE) return;
    s_sw_warned = 1;
    fprintf(stdout, "psxrecomp: vram extension: the software rasterizer does "
                    "not redirect draws (no extension plane); use opengl or "
                    "vulkan for a wide framebuffer\n");
}

int vram_ext_init(void) {
    if (s_inited) return s_enabled;
    s_inited = 1;
    s_enabled = 0;

    const char *env = getenv("PSX_VRAM_EXTENSION");
    if (env && *env) {
        s_enabled = (*env == '1' || *env == 't' || *env == 'T' ||
                     *env == 'y' || *env == 'Y');
    } else if (!cfg_video_bool("vram_extension", &s_enabled)) {
        s_enabled = 0;
    }
    if (s_enabled)
        fprintf(stdout, "psxrecomp: vram extension on (host VRAM %dx512, "
                        "guest columns %d..%d redirected)\n",
                VRAM_EXT_HOST_W, VRAM_EXT_X0, VRAM_EXT_X0 + VRAM_EXT_COLS - 1);
    warn_if_software();
    return s_enabled;
}

int vram_ext_enabled(void) {
    if (!s_inited) vram_ext_init();
    return s_enabled;
}

void vram_ext_set_backend(int backend) {
    s_backend = backend;
    warn_if_software();
}

int vram_ext_backend(void) { return s_backend; }

int vram_ext_driving(void) {
    if (!vram_ext_enabled()) return 0;
    return s_backend == VRAM_EXT_BACKEND_OPENGL || s_backend == VRAM_EXT_BACKEND_VULKAN;
}

/* ------------------------------------------------------------- region ---- */

static int s_reg_on;
static int s_reg_x0, s_reg_y0, s_reg_x1, s_reg_y1;

void vram_ext_set_region(int on, int x0, int y0, int x1, int y1) {
    s_reg_on = on ? 1 : 0;
    s_reg_x0 = x0; s_reg_y0 = y0; s_reg_x1 = x1; s_reg_y1 = y1;
    /* The plane is finite: never let a wider display push the redirect past it
     * (a >704-wide framebuffer would need a bigger VRAM_EXT_COLS). */
    if (s_reg_x1 > VRAM_EXT_X0 + VRAM_EXT_COLS - 1)
        s_reg_x1 = VRAM_EXT_X0 + VRAM_EXT_COLS - 1;
    if (s_reg_x0 < 0) s_reg_x0 = 0;
    if (s_reg_y0 < 0) s_reg_y0 = 0;
    if (s_reg_y1 > 511) s_reg_y1 = 511;
}

int vram_ext_region(int *x0, int *y0, int *x1, int *y1) {
    if (!s_reg_on || !vram_ext_driving()) return 0;
    if (x0) *x0 = s_reg_x0;
    if (y0) *y0 = s_reg_y0;
    if (x1) *x1 = s_reg_x1;
    if (y1) *y1 = s_reg_y1;
    return 1;
}

int vram_ext_plane_index(int x, int y) {
    if (!s_reg_on || !vram_ext_driving()) return -1;
    if (x < VRAM_EXT_X0 || x > s_reg_x1) return -1;
    if (y < s_reg_y0 || y > s_reg_y1) return -1;
    if (y < 0 || y >= 512) return -1;
    return y * VRAM_EXT_COLS + (x - VRAM_EXT_X0);
}

int vram_ext_map_x(int x, int y) {
    return vram_ext_plane_index(x, y) >= 0 ? x + VRAM_EXT_OFFSET : x;
}

/* -------------------------------------------------------- host plane ----- */

/* Zero-initialised: the extension is scratch, not guest state, and is never
 * part of a savestate (the guest plane stays canonical). NOT named s_host:
 * <windows.h> -> <inaddr.h> defines s_host as a struct macro. */
static uint16_t s_host_plane[VRAM_EXT_COLS * 512];

uint16_t *vram_ext_host(void) { return s_host_plane; }

/* ---------------------------------------------------------- counters ----- */

static struct {
    uint64_t remapped_pixels;
    uint64_t readback_bytes;
    uint64_t gate_off_frames;
    uint64_t presents;
    int      present_width;
} s_stats;

void vram_ext_note_remap(uint64_t pixels)       { s_stats.remapped_pixels += pixels; }
void vram_ext_note_readback(uint64_t bytes)     { s_stats.readback_bytes += bytes; }
void vram_ext_note_gate_off(uint64_t pixels)    { s_stats.gate_off_frames += pixels; }
void vram_ext_note_present(int width) {
    if (width > 0) s_stats.present_width = width;
    s_stats.presents++;
}
void vram_ext_reset_stats(void) { memset(&s_stats, 0, sizeof s_stats); }

int vram_ext_debug_json(char *out, int cap) {
    if (!out || cap <= 0) return 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int on = s_reg_on && vram_ext_driving();
    if (on) { x0 = s_reg_x0; y0 = s_reg_y0; x1 = s_reg_x1; y1 = s_reg_y1; }
    return snprintf(out, (size_t)cap,
                    "{\"enabled\":%d,\"driving\":%d,\"backend\":%d,"
                    "\"host_w\":%d,\"ext_x0\":%d,\"ext_cols\":%d,\"offset\":%d,"
                    "\"region\":[%d,%d,%d,%d],\"region_on\":%d,"
                    "\"remapped_pixels\":%llu,\"readback_bytes\":%llu,"
                    "\"gate_off_frames\":%llu,\"presents\":%llu,"
                    "\"present_width\":%d}",
                    vram_ext_enabled(), vram_ext_driving(), s_backend,
                    VRAM_EXT_HOST_W, VRAM_EXT_X0, VRAM_EXT_COLS, VRAM_EXT_OFFSET,
                    x0, y0, x1, y1, on,
                    (unsigned long long)s_stats.remapped_pixels,
                    (unsigned long long)s_stats.readback_bytes,
                    (unsigned long long)s_stats.gate_off_frames,
                    (unsigned long long)s_stats.presents,
                    s_stats.present_width);
}
