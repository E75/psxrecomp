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

/* Two things, and the difference is the fix for the striped-HUD regression:
 *
 *   s_reg_on   - the LIVE gate: the draw area being set up right now is the
 *                wide display buffer. gpu.c toggles this per GP0(E3/E4) /
 *                GP1(06h) write, so it is true and false several times inside
 *                one frame (the match re-asserts its env every flip, and the
 *                register arm widens the clip in a second write).
 *   s_arc_on   - the STICKY geometry: the last rect that passed the span test.
 *                It is what the pack (render target -> raw mirror) and the
 *                display read split with.
 *
 * Using the live gate for the pack was the regression: a pack that ran while
 * the gate happened to be closed packed the whole dirty rect - including the
 * wide framebuffer's overflow columns 512..681 - into GUEST VRAM, overwriting
 * the texture pages and CLUTs that paletted sprites sample (vertical stripes
 * in HUD text and kits). The geometry persists exactly like the learned bands
 * (they answer the same question for rows) and is dropped only on a GPU reset
 * by vram_ext_reset_bands; a 4:3 screen cannot draw past its own clip, so a
 * stale arc cannot capture a 4:3 frame's pixels. */
static int s_reg_on;
static int s_reg_x0, s_reg_y0, s_reg_x1, s_reg_y1;
static int s_arc_on;
static int s_arc_x0, s_arc_y0, s_arc_x1, s_arc_y1;

/* -------------------------------------------------------------- bands ---- */
/* Every row band that has been a display buffer with the current display
 * geometry. The redirect covers all of them: a frame's pack can run after the
 * game flipped to the other band, and packing the old band at guest columns
 * would put the framebuffer straight back on the texture pages. */
#define VRAM_EXT_BANDS 4
static struct { int y0, y1; } s_band[VRAM_EXT_BANDS];
static int s_band_n;

void vram_ext_learn_band(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > 511) y1 = 511;
    if (y1 < y0) return;
    for (int i = 0; i < s_band_n; i++)
        if (y0 >= s_band[i].y0 && y1 <= s_band[i].y1) return;
    if (s_band_n < VRAM_EXT_BANDS) {
        s_band[s_band_n].y0 = y0;
        s_band[s_band_n].y1 = y1;
        s_band_n++;
        return;
    }
    /* Table full: collapse to one band covering everything learned so far. */
    int lo = y0, hi = y1;
    for (int i = 0; i < s_band_n; i++) {
        if (s_band[i].y0 < lo) lo = s_band[i].y0;
        if (s_band[i].y1 > hi) hi = s_band[i].y1;
    }
    s_band[0].y0 = lo;
    s_band[0].y1 = hi;
    s_band_n = 1;
}

/* Bands and the sticky framebuffer geometry live and die together: gpu.c calls
 * this on a GPU reset (savestate load, soft reset), where the plane is scratch
 * and nothing from the old display may be redirected any more. */
void vram_ext_reset_bands(void) { s_band_n = 0; vram_ext_clear_region(); }

int vram_ext_band_rows(int *y0, int *y1) {
    if (s_band_n <= 0) return 0;
    int lo = s_band[0].y0, hi = s_band[0].y1;
    for (int i = 1; i < s_band_n; i++) {
        if (s_band[i].y0 < lo) lo = s_band[i].y0;
        if (s_band[i].y1 > hi) hi = s_band[i].y1;
    }
    if (y0) *y0 = lo;
    if (y1) *y1 = hi;
    return 1;
}

void vram_ext_set_region(int on, int x0, int y0, int x1, int y1) {
    s_reg_on = on ? 1 : 0;
    if (!on) return;                 /* a non-framebuffer draw: keep the arc */
    /* The plane is finite: never let a wider display push the redirect past it
     * (a >704-wide framebuffer would need a bigger VRAM_EXT_COLS). */
    if (x1 > VRAM_EXT_X0 + VRAM_EXT_COLS - 1)
        x1 = VRAM_EXT_X0 + VRAM_EXT_COLS - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (y1 > 511) y1 = 511;
    s_reg_x0 = x0; s_reg_y0 = y0; s_reg_x1 = x1; s_reg_y1 = y1;
    s_arc_on = 1;
    s_arc_x0 = x0; s_arc_y0 = y0; s_arc_x1 = x1; s_arc_y1 = y1;
}

void vram_ext_clear_region(void) {
    s_reg_on = 0;
    s_arc_on = 0;
}

int vram_ext_region(int *x0, int *y0, int *x1, int *y1) {
    if (!s_arc_on || !vram_ext_driving()) return 0;
    if (x0) *x0 = s_arc_x0;
    if (y0) *y0 = s_arc_y0;
    if (x1) *x1 = s_arc_x1;
    if (y1) *y1 = s_arc_y1;
    return 1;
}

int vram_ext_draw_gate(int *x0, int *y0, int *x1, int *y1) {
    if (!s_reg_on || !vram_ext_driving()) return 0;
    if (x0) *x0 = s_reg_x0;
    if (y0) *y0 = s_reg_y0;
    if (x1) *x1 = s_reg_x1;
    if (y1) *y1 = s_reg_y1;
    return 1;
}

int vram_ext_plane_index(int x, int y) {
    if (!s_arc_on || !vram_ext_driving()) return -1;
    if (x < VRAM_EXT_X0 || x > s_arc_x1) return -1;
    if (y < s_arc_y0 || y > s_arc_y1) return -1;
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
    uint64_t pack_redirect_pixels;     /* redirected by the pack (GPU path) */
    uint64_t cpu_redirect_pixels;      /* redirected by the CPU rasterizer */
    uint64_t texture_reads_remapped;   /* MUST stay 0: textures are canonical */
    uint64_t guest_pack_pixels;        /* MUST stay 0: fb columns -> guest VRAM */
    uint64_t readback_bytes;
    uint64_t gate_off_frames;
    uint64_t presents;
    uint64_t pack_calls;
    uint64_t pack_passes;
    uint64_t pack_pixels;
    double   pack_ms;                  /* CPU wall inside the pack calls */
    uint64_t gl_bypass_frames;
    int      gl_bypass_reason;
    int      pack_max_x1;              /* widest pack rect right edge seen */
    int      present_width;
} s_stats;

void vram_ext_note_readback(uint64_t bytes)     { s_stats.readback_bytes += bytes; }
void vram_ext_note_gate_off(uint64_t frames)    { s_stats.gate_off_frames += frames; }
void vram_ext_note_present(int width) {
    if (width > 0) s_stats.present_width = width;
    s_stats.presents++;
}

/* A texture fetch must resolve against the canonical guest plane. The GPU
 * samplers use guest coordinates only (texelFetch with `& 1023`) and every
 * upload path writes guest coordinates, so this stays 0; a path that remaps a
 * texture coordinate reports 1 and `texture_reads_ok` flips false. */
void vram_ext_note_texture_read(int remapped) {
    if (remapped) s_stats.texture_reads_remapped++;
}

/* Split one pack rect into "served from the extension" (present-side) and
 * "framebuffer column written into guest VRAM" (a redirect miss: the pack
 * overwrote a texture page). The second number is the assertion. */
void vram_ext_note_pack_rect(int x0, int y0, int x1, int y1) {
    if (x0 > x1 || y0 > y1) return;
    s_stats.pack_calls++;
    if (x1 > s_stats.pack_max_x1) s_stats.pack_max_x1 = x1;
    int gx0 = x0 > VRAM_EXT_X0 ? x0 : VRAM_EXT_X0;
    /* The framebuffer spans x0..region_x1. Columns past the display's right
     * edge (682..1023) are texture pages - a copy or render-to-texture
     * destination there is correctly guest - so they must not trip the
     * assertion. Without this clamp the GL pack rect (which spans the full
     * 1024-wide dirty rect) made guest_pack_pixels permanently non-zero. */
    int gx1 = x1;
    if (s_arc_on && gx1 > s_arc_x1) gx1 = s_arc_x1;
    if (gx0 > gx1) {
        /* One-shot: the pack ran but its rect never reached the split, so the
         * framebuffer's overflow columns were not redirected. Names the rect,
         * the region and the bands for the next run. */
        static int logged;
        if (!logged && s_arc_on) {
            logged = 1;
            fprintf(stderr, "psxrecomp: vram extension: pack rect [%d,%d,%d,%d] "
                            "below the split (%d); region [%d,%d,%d,%d] bands=%d\n",
                    x0, y0, x1, y1, VRAM_EXT_X0,
                    s_arc_x0, s_arc_y0, s_arc_x1, s_arc_y1, s_band_n);
        }
        return;
    }
    /* Rows the redirect covers: the union of the learned bands (they can
     * overlap - a band learned at 0..239 and another at 0..479 - and a per-band
     * loop counted the shared rows twice, which made this assertion fire on a
     * correct pack). The arc always covers the union. */
    uint64_t in_band = 0;
    int by0, by1;
    if (vram_ext_band_rows(&by0, &by1)) {
        int cy0 = y0 > by0 ? y0 : by0;
        int cy1 = y1 < by1 ? y1 : by1;
        if (cy0 <= cy1)
            in_band = (uint64_t)(gx1 - gx0 + 1) * (uint64_t)(cy1 - cy0 + 1);
    }
    uint64_t redirected = 0;
    if (s_arc_on && vram_ext_driving()) {
        int cy0 = y0 > s_arc_y0 ? y0 : s_arc_y0;
        int cy1 = y1 < s_arc_y1 ? y1 : s_arc_y1;
        int cx1 = gx1 < s_arc_x1 ? gx1 : s_arc_x1;
        if (cx1 >= gx0 && cy0 <= cy1)
            redirected = (uint64_t)(cx1 - gx0 + 1) * (uint64_t)(cy1 - cy0 + 1);
    }
    s_stats.pack_redirect_pixels += redirected;
    if (in_band > redirected) {
        static int logged_rows;
        s_stats.guest_pack_pixels += in_band - redirected;
        if (!logged_rows && s_arc_on) {
            logged_rows = 1;
            fprintf(stderr, "psxrecomp: vram extension: pack rect [%d,%d,%d,%d] "
                            "writes %llu framebuffer px into guest VRAM; region "
                            "[%d,%d,%d,%d] bands=%d\n",
                    x0, y0, x1, y1,
                    (unsigned long long)(in_band - redirected),
                    s_arc_x0, s_arc_y0, s_arc_x1, s_arc_y1, s_band_n);
        }
    }
}

void vram_ext_note_cpu_redirect(uint64_t pixels) {
    s_stats.cpu_redirect_pixels += pixels;
}

void vram_ext_note_pack_passes(uint64_t passes, uint64_t pixels) {
    s_stats.pack_passes += passes;
    s_stats.pack_pixels += pixels;
}

void vram_ext_note_pack_time(double ms) {
    if (ms > 0.0) s_stats.pack_ms += ms;
}

/* A frame the GL GPU raster did not draw. One query answers "is the GPU path
 * actually running?"; the first frame per reason also lands in the log. */
void vram_ext_note_gl_bypass(int reason) {
    static const char *const why[] = {
        "none", "no GL context", "GL raster not initialised",
        "depth24 scanout", "dual-raster CPU authority",
        "HD native authority", "frame-generation draw"
    };
    static unsigned seen;
    s_stats.gl_bypass_frames++;
    s_stats.gl_bypass_reason = reason;
    if (reason < 1 || reason >= (int)(sizeof why / sizeof why[0])) return;
    if (seen & (1u << reason)) return;
    seen |= 1u << reason;
    fprintf(stderr, "psxrecomp: GL GPU raster bypassed for VRAM: %s (the CPU "
                    "rasterizer owns VRAM, the GPU pack does not run)\n", why[reason]);
}

void vram_ext_reset_stats(void) { memset(&s_stats, 0, sizeof s_stats); }

/* The debug reply path must stay small: `gpu_state` is a big object and a reply
 * that blocks the emulation thread long enough trips the starvation watchdog
 * (the query closed the connection at kickoff, four times). This object is
 * ~430 bytes with every counter populated, so the caller's buffer must hold it
 * whole - a truncated object is not valid JSON (the 384-byte buffer cut
 * "gl_bypass_reason":0 and failed every harness row that reads gpu_state). */
int vram_ext_debug_json(char *out, int cap) {
    if (!out || cap <= 0) return 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int on = s_arc_on && vram_ext_driving();
    if (on) { x0 = s_arc_x0; y0 = s_arc_y0; x1 = s_arc_x1; y1 = s_arc_y1; }
    int by0 = 0, by1 = 0;
    int bands = vram_ext_band_rows(&by0, &by1);
    return snprintf(out, (size_t)cap,
                    "{\"enabled\":%d,\"driving\":%d,\"backend\":%d,"
                    "\"region_on\":%d,\"region\":[%d,%d,%d,%d],"
                    "\"gate_open\":%d,"
                    "\"bands\":%d,\"band_rows\":[%d,%d],"
                    "\"present_reads_remapped\":%llu,"
                    "\"pack_redirect_pixels\":%llu,"
                    "\"cpu_redirect_pixels\":%llu,"
                    "\"texture_reads_remapped\":%llu,"
                    "\"guest_pack_pixels\":%llu,"
                    "\"gate_off_frames\":%llu,\"present_width\":%d,"
                    "\"pack_calls\":%llu,\"pack_max_x1\":%d,"
                    "\"pack_passes\":%llu,\"pack_pixels\":%llu,\"pack_ms\":%.3f,"
                    "\"gl_bypass_frames\":%llu,\"gl_bypass_reason\":%d}",
                    vram_ext_enabled(), vram_ext_driving(), s_backend,
                    on, x0, y0, x1, y1, s_reg_on,
                    bands, by0, by1,
                    (unsigned long long)(s_stats.pack_redirect_pixels +
                                         s_stats.cpu_redirect_pixels),
                    (unsigned long long)s_stats.pack_redirect_pixels,
                    (unsigned long long)s_stats.cpu_redirect_pixels,
                    (unsigned long long)s_stats.texture_reads_remapped,
                    (unsigned long long)s_stats.guest_pack_pixels,
                    (unsigned long long)s_stats.gate_off_frames,
                    s_stats.present_width,
                    (unsigned long long)s_stats.pack_calls,
                    s_stats.pack_max_x1,
                    (unsigned long long)s_stats.pack_passes,
                    (unsigned long long)s_stats.pack_pixels,
                    s_stats.pack_ms,
                    (unsigned long long)s_stats.gl_bypass_frames,
                    s_stats.gl_bypass_reason);
}
