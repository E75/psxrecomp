/* vfx_config.c - [video] volumetrics/light-shaft config (docs/EFFECTS.md).
 *
 * Runtime-side game.toml reader: config_loader.h turns game.toml into the
 * generated game-config struct at build time, but the effects below are a
 * runtime-only host feature that must work with an already-shipped game.toml
 * and be flippable without a regen. vram_ext.c reads its own [video] key the
 * same way; this module generalises that reader to strings, floats and ints.
 *
 * Precedence: PSX_VFX* environment override > game.toml [video] > default off.
 * Nothing here allocates or logs unless an effect is actually on. */

#include "vfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define CFG_PATH_MAX 1024

static int   s_inited;
static int   s_mode;                    /* VFX_* bitmask */
static float s_fog_density = 0.35f;
static float s_fog_color[3] = { 0.722f, 0.784f, 0.847f };  /* #B8C8D8 */
static float s_shaft_strength = 0.5f;
static int   s_shaft_samples = 24;
static float s_shaft_uv[2] = { 0.5f, 0.18f };   /* projected light position */
static int   s_active_fog, s_active_shafts;
static float s_last_cost_ms;

/* ------------------------------------------------------------- config ---- */

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

/* Raw value text of `key` in the [video] block, unquoted, trimmed. */
static int cfg_video_str(const char *key, char *out, size_t cap) {
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
        size_t len;
        if (*v == '"' || *v == '\'') {
            char quote = *v++;
            char *end = strchr(v, quote);
            if (!end) continue;
            len = (size_t)(end - v);
        } else {
            char *end = v;
            while (*end && *end != '\r' && *end != '\n' && *end != '#') ++end;
            while (end > v && (end[-1] == ' ' || end[-1] == '\t')) --end;
            len = (size_t)(end - v);
        }
        if (len >= cap) len = cap - 1;
        memcpy(out, v, len);
        out[len] = '\0';
        got = 1;
    }
    fclose(f);
    return got;
}

static int cfg_video_float(const char *key, float *out) {
    char tmp[64];
    if (!cfg_video_str(key, tmp, sizeof tmp)) return 0;
    char *end = NULL;
    double v = strtod(tmp, &end);
    if (end == tmp) return 0;
    *out = (float)v;
    return 1;
}

static int cfg_video_int(const char *key, int *out) {
    char tmp[64];
    if (!cfg_video_str(key, tmp, sizeof tmp)) return 0;
    char *end = NULL;
    long v = strtol(tmp, &end, 10);
    if (end == tmp) return 0;
    *out = (int)v;
    return 1;
}

static int parse_mode(const char *s) {
    if (!s || !s[0]) return 0;
    if (!strcmp(s, "both")) return VFX_FOG | VFX_SHAFTS;
    if (!strcmp(s, "fog")) return VFX_FOG;
    if (!strcmp(s, "lightshafts")) return VFX_SHAFTS;
    if (!strcmp(s, "shafts")) return VFX_SHAFTS;
    return 0;                          /* "off" and anything unknown */
}

/* "#RRGGBB" / "RRGGBB" -> linear-ish 0..1 RGB (sRGB byte / 255). */
static int parse_color(const char *s, float out[3]) {
    if (!s || !s[0]) return 0;
    if (*s == '#') ++s;
    if (strlen(s) < 6) return 0;
    unsigned v = 0;
    for (int i = 0; i < 6; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return 0;
        v = v * 16u + d;
    }
    out[0] = (float)((v >> 16) & 0xFF) / 255.0f;
    out[1] = (float)((v >> 8) & 0xFF) / 255.0f;
    out[2] = (float)(v & 0xFF) / 255.0f;
    return 1;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void vfx_init(void) {
    if (s_inited) return;
    s_inited = 1;

    char tmp[64];
    const char *env;

    if (cfg_video_str("volumetrics", tmp, sizeof tmp)) s_mode = parse_mode(tmp);
    if ((env = getenv("PSX_VFX")) && *env) s_mode = parse_mode(env);

    if (cfg_video_float("fog_density", &s_fog_density) == 0) s_fog_density = 0.35f;
    if ((env = getenv("PSX_VFX_FOG_DENSITY")) && *env)
        s_fog_density = (float)strtod(env, NULL);
    s_fog_density = clampf(s_fog_density, 0.0f, 1.0f);

    if (!cfg_video_str("fog_color", tmp, sizeof tmp) || !parse_color(tmp, s_fog_color)) {
        s_fog_color[0] = 0.722f; s_fog_color[1] = 0.784f; s_fog_color[2] = 0.847f;
    }
    if ((env = getenv("PSX_VFX_FOG_COLOR")) && *env) parse_color(env, s_fog_color);

    if (cfg_video_float("lightshaft_strength", &s_shaft_strength) == 0) s_shaft_strength = 0.5f;
    if ((env = getenv("PSX_VFX_SHAFT_STRENGTH")) && *env)
        s_shaft_strength = (float)strtod(env, NULL);
    s_shaft_strength = clampf(s_shaft_strength, 0.0f, 2.0f);

    if (cfg_video_int("lightshaft_samples", &s_shaft_samples) == 0) s_shaft_samples = 24;
    if ((env = getenv("PSX_VFX_SHAFT_SAMPLES")) && *env)
        s_shaft_samples = (int)strtol(env, NULL, 10);
    /* Radial blur taps: even, 2..64. Odd counts would leave the blur centre
     * off a tap and cost a second fetch; 64 is the point where the pass stops
     * being cheap at 1440p (docs/EFFECTS.md cost model). */
    if (s_shaft_samples < 2) s_shaft_samples = 2;
    if (s_shaft_samples > 64) s_shaft_samples = 64;
    s_shaft_samples &= ~1;
    if (s_shaft_samples < 2) s_shaft_samples = 2;

    if (s_mode & VFX_FOG)
        fprintf(stdout, "psxrecomp: vfx: fog on (density %.2f, colour #%02X%02X%02X)\n",
                (double)s_fog_density,
                (unsigned)(s_fog_color[0] * 255.0f + 0.5f),
                (unsigned)(s_fog_color[1] * 255.0f + 0.5f),
                (unsigned)(s_fog_color[2] * 255.0f + 0.5f));
    if (s_mode & VFX_SHAFTS)
        fprintf(stdout, "psxrecomp: vfx: light shafts on (strength %.2f, %d taps)\n",
                (double)s_shaft_strength, s_shaft_samples);
}

int   vfx_mode(void)           { if (!s_inited) vfx_init(); return s_mode; }
int   vfx_fog_enabled(void)    { return (vfx_mode() & VFX_FOG) != 0; }
int   vfx_shafts_enabled(void) { return (vfx_mode() & VFX_SHAFTS) != 0; }
float vfx_fog_density(void)    { if (!s_inited) vfx_init(); return s_fog_density; }
float vfx_shaft_strength(void) { if (!s_inited) vfx_init(); return s_shaft_strength; }
int   vfx_shaft_samples(void)  { if (!s_inited) vfx_init(); return s_shaft_samples; }

void vfx_fog_color(float out_rgb[3]) {
    if (!s_inited) vfx_init();
    out_rgb[0] = s_fog_color[0];
    out_rgb[1] = s_fog_color[1];
    out_rgb[2] = s_fog_color[2];
}

void vfx_shaft_light_uv(float out_uv[2]) {
    if (!s_inited) vfx_init();
    out_uv[0] = s_shaft_uv[0];
    out_uv[1] = s_shaft_uv[1];
}

void  vfx_set_active(int fog, int shafts) { s_active_fog = fog ? 1 : 0; s_active_shafts = shafts ? 1 : 0; }
int   vfx_active_fog(void)    { return s_active_fog; }
int   vfx_active_shafts(void) { return s_active_shafts; }
void  vfx_report_cost(float ms) { s_last_cost_ms = ms; }
float vfx_last_cost_ms(void)  { return s_last_cost_ms; }

int vfx_state_json(char *out, int cap) {
    if (!s_inited) vfx_init();
    const char *mode = "off";
    if (s_mode == (VFX_FOG | VFX_SHAFTS)) mode = "both";
    else if (s_mode == VFX_FOG) mode = "fog";
    else if (s_mode == VFX_SHAFTS) mode = "lightshafts";
    return snprintf(out, cap,
        "{\"enabled\":%d,\"mode\":\"%s\",\"fog\":%d,\"lightshafts\":%d,"
        "\"samples\":%d,\"fog_density\":%.3f,\"fog_color\":\"#%02X%02X%02X\","
        "\"lightshaft_strength\":%.3f,\"lightshaft_samples\":%d,"
        "\"active_fog\":%d,\"active_lightshafts\":%d,"
        "\"light_uv\":[%.3f,%.3f],\"last_ms\":%.3f,\"budget_ms\":1.5,"
        "\"backend\":\"vulkan\"}",
        s_mode != 0, mode,
        (s_mode & VFX_FOG) != 0, (s_mode & VFX_SHAFTS) != 0,
        s_shaft_samples,
        (double)s_fog_density,
        (unsigned)(s_fog_color[0] * 255.0f + 0.5f),
        (unsigned)(s_fog_color[1] * 255.0f + 0.5f),
        (unsigned)(s_fog_color[2] * 255.0f + 0.5f),
        (double)s_shaft_strength, s_shaft_samples,
        s_active_fog, s_active_shafts,
        (double)s_shaft_uv[0], (double)s_shaft_uv[1],
        (double)s_last_cost_ms);
}
