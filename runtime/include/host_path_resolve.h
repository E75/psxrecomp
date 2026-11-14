/*
 * host_path_resolve.h — one deterministic rule for every asset/config path.
 *
 * docs/ASSET_PATHS.md is the reference. The rule:
 *
 *   absolute            -> used as written
 *   otherwise, in order -> <game.toml project dir>/rel
 *                          <exe dir>/rel
 *                          <exe dir>/../rel
 *                          <cwd>/rel
 *
 * The first candidate that exists wins. When none exists the first candidate is
 * returned, so a report always shows where the artefact belongs instead of an
 * empty string. This replaces the per-module guesses (audio_native.c: cwd, exe,
 * exe/..; main.cpp: config-dir absolute; HD pack: env var only), which made the
 * same key resolve to different files depending on where the process started.
 *
 * Header-only C99 (also valid C++) so the C runtime modules and main.cpp share
 * one implementation without a new translation unit.
 */

#ifndef PSX_HOST_PATH_RESOLVE_H
#define PSX_HOST_PATH_RESOLVE_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#define PSX_PATH_RESOLVE_MAX 1024

/* The four anchors, in rule order. Empty entries are skipped. */
typedef struct PsxPathRoots {
    char config_dir[PSX_PATH_RESOLVE_MAX];  /* game.toml's project directory */
    char exe_dir[PSX_PATH_RESOLVE_MAX];
    char exe_parent[PSX_PATH_RESOLVE_MAX];
    char cwd[PSX_PATH_RESOLVE_MAX];
} PsxPathRoots;

/* "/x", "\x", "\\server\share" and "C:/x" / "C:x" are all host-absolute. */
static inline int psx_path_is_absolute(const char *path) {
    if (!path || !path[0]) return 0;
    if (path[0] == '/' || path[0] == '\\') return 1;
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

static inline int psx_path_exists(const char *path) {
    struct stat st;
    if (!path || !path[0]) return 0;
    return stat(path, &st) == 0;
}

/* Collapse "." and "dir/.." segments so logs and the debug report carry a
 * stable, comparable spelling. Separators become '/'; a Windows drive prefix
 * and a leading root survive. */
static inline void psx_path_normalize(char *path) {
    char tmp[PSX_PATH_RESOLVE_MAX];
    size_t i = 0, o = 0, n;
    int rooted = 0;
    if (!path) return;
    n = strlen(path);
    if (n + 1u > sizeof tmp) return;
    if (n && (path[0] == '/' || path[0] == '\\')) {
        rooted = 1;
        tmp[o++] = '/';
        i = 1;
    }
    while (i < n) {
        size_t start, len;
        while (i < n && (path[i] == '/' || path[i] == '\\')) ++i;
        start = i;
        while (i < n && path[i] != '/' && path[i] != '\\') ++i;
        len = i - start;
        if (len == 0) break;
        if (len == 1 && path[start] == '.') continue;
        if (len == 2 && path[start] == '.' && path[start + 1] == '.') {
            size_t pop = o;
            while (pop > 0 && tmp[pop - 1] != '/') --pop;
            if (pop > 0) --pop;                    /* drop the separator */
            if (pop == 2 && tmp[1] == ':') pop = 2; /* never pop "C:" */
            o = pop;
            if (rooted && o == 0) o = 1;
            continue;
        }
        if (o > 0 && tmp[o - 1] != '/') tmp[o++] = '/';
        memcpy(tmp + o, path + start, len);
        o += len;
    }
    if (o == 0) { tmp[o++] = '.'; }
    if (o + 1u > sizeof tmp) return;
    tmp[o] = '\0';
    snprintf(path, PSX_PATH_RESOLVE_MAX, "%s", tmp);
}

static inline void psx_path_join(char *out, size_t cap, const char *dir,
                          const char *rel) {
    size_t n;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!dir || !dir[0]) {
        snprintf(out, cap, "%s", rel ? rel : "");
        return;
    }
    n = strlen(dir);
    snprintf(out, cap, "%s%s%s", dir,
             (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/",
             rel ? rel : "");
}

static inline void psx_path_roots_init(PsxPathRoots *roots, const char *config_dir,
                                const char *exe_dir) {
    if (!roots) return;
    memset(roots, 0, sizeof *roots);
    if (config_dir && config_dir[0])
        snprintf(roots->config_dir, sizeof roots->config_dir, "%s", config_dir);
    if (exe_dir && exe_dir[0]) {
        snprintf(roots->exe_dir, sizeof roots->exe_dir, "%s", exe_dir);
        snprintf(roots->exe_parent, sizeof roots->exe_parent, "%s/..", exe_dir);
    }
    /* Normalize only what was set: psx_path_normalize("") spells ".", which
     * would turn an unset config_dir into a first candidate relative to the
     * cwd - ahead of the exe anchors. That made a bare `assets/audio` resolve
     * to an empty build-tree directory instead of the real one. */
    if (roots->config_dir[0]) psx_path_normalize(roots->config_dir);
    if (roots->exe_dir[0]) psx_path_normalize(roots->exe_dir);
    if (roots->exe_parent[0]) psx_path_normalize(roots->exe_parent);
#ifdef _WIN32
    if (_getcwd(roots->cwd, sizeof roots->cwd) == NULL) roots->cwd[0] = '\0';
#else
    if (getcwd(roots->cwd, sizeof roots->cwd) == NULL) roots->cwd[0] = '\0';
#endif
    if (roots->cwd[0]) psx_path_normalize(roots->cwd);
}

/* Resolve one configured path. `out` always receives the result (empty only for
 * an empty input). Returns 1 when the result exists, 0 when nothing on the
 * chain exists (out then holds the first candidate). */
static inline int psx_path_resolve(const PsxPathRoots *roots, const char *configured,
                            char *out, size_t cap) {
    const char *dirs[4];
    char first[PSX_PATH_RESOLVE_MAX];
    int i;
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    first[0] = '\0';
    if (!configured || !configured[0]) return 0;
    if (psx_path_is_absolute(configured)) {
        snprintf(out, cap, "%s", configured);
        psx_path_normalize(out);
        return psx_path_exists(out);
    }
    dirs[0] = roots ? roots->config_dir : NULL;
    dirs[1] = roots ? roots->exe_dir : NULL;
    dirs[2] = roots ? roots->exe_parent : NULL;
    dirs[3] = roots ? roots->cwd : NULL;
    for (i = 0; i < 4; ++i) {
        char candidate[PSX_PATH_RESOLVE_MAX];
        if (!dirs[i] || !dirs[i][0]) continue;
        psx_path_join(candidate, sizeof candidate, dirs[i], configured);
        psx_path_normalize(candidate);
        if (!first[0]) snprintf(first, sizeof first, "%s", candidate);
        if (psx_path_exists(candidate)) {
            snprintf(out, cap, "%s", candidate);
            return 1;
        }
    }
    snprintf(out, cap, "%s", first);
    return 0;
}

#endif /* PSX_HOST_PATH_RESOLVE_H */
