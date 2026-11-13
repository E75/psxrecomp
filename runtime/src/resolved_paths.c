/*
 * resolved_paths.c — the table behind the `resolved_paths` debug command.
 *
 * Every asset/config key the runtime resolves is recorded once, in resolution
 * order, with the configured value, the absolute resolved path, whether that
 * path exists and where the value came from (config / env / default / cli).
 * docs/ASSET_PATHS.md is the reference. The acceptance harness reads the command
 * so a live asset row can only FAIL on a genuine absence instead of on a guess
 * about the process working directory.
 *
 * Read-only: this module formats what the resolvers already decided.
 */

#include "resolved_paths.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "host_path_resolve.h"

#define RESOLVED_PATHS_MAX 16

typedef struct ResolvedPathRow {
    char key[48];
    char configured[PSX_PATH_RESOLVE_MAX];
    char resolved[PSX_PATH_RESOLVE_MAX];
    char source[16];
    int  exists;
} ResolvedPathRow;

static ResolvedPathRow g_rows[RESOLVED_PATHS_MAX];
static int             g_count;
static PsxPathRoots    g_roots;
static char            g_config[PSX_PATH_RESOLVE_MAX];

void psx_resolved_paths_set_roots(const char* config_path,
                                  const PsxPathRoots* roots) {
    snprintf(g_config, sizeof g_config, "%s", config_path ? config_path : "");
    if (roots) g_roots = *roots;
}

const char* psx_resolved_paths_get(const char* key) {
    int i;
    if (!key) return NULL;
    for (i = 0; i < g_count; ++i)
        if (strcmp(g_rows[i].key, key) == 0) return g_rows[i].resolved;
    return NULL;
}

void psx_resolved_paths_note(const char* key, const char* configured,
                             const char* resolved, const char* source) {
    ResolvedPathRow* row;
    if (!key || !key[0] || !resolved || !resolved[0]) return;
    if (psx_resolved_paths_get(key)) return;      /* resolved once, logged once */
    if (g_count >= RESOLVED_PATHS_MAX) return;
    row = &g_rows[g_count++];
    snprintf(row->key, sizeof row->key, "%s", key);
    snprintf(row->configured, sizeof row->configured, "%s",
             configured && configured[0] ? configured : resolved);
    snprintf(row->resolved, sizeof row->resolved, "%s", resolved);
    snprintf(row->source, sizeof row->source, "%s",
             source && source[0] ? source : "config");
    row->exists = psx_path_exists(row->resolved);
    printf("psxrecomp: path %s = %s (configured \"%s\", %s)\n", row->key,
           row->resolved, row->configured, row->exists ? "found" : "MISSING");
}

static void append(char* out, int cap, int* used, const char* fmt, ...) {
    va_list ap;
    int room;
    if (*used >= cap) return;
    room = cap - *used;
    va_start(ap, fmt);
    *used += vsnprintf(out + *used, (size_t)room, fmt, ap);
    va_end(ap);
    if (*used > cap) *used = cap;   /* truncated: json() then reports 0 */
}

static void append_escaped(char* out, int cap, int* used, const char* value) {
    char escaped[PSX_PATH_RESOLVE_MAX * 2];
    size_t o = 0;
    const char* p;
    for (p = value ? value : ""; *p && o + 2 < sizeof escaped; ++p) {
        if (*p == '"' || *p == '\\') escaped[o++] = '\\';
        escaped[o++] = *p;
    }
    escaped[o] = '\0';
    append(out, cap, used, "%s", escaped);
}

int psx_resolved_paths_json(char* out, int cap) {
    static const char* names[4] = {"config_dir", "exe_dir", "exe_parent", "cwd"};
    const char* values[4];
    int used = 0, i;
    if (!out || cap <= 0) return 0;
    values[0] = g_roots.config_dir;
    values[1] = g_roots.exe_dir;
    values[2] = g_roots.exe_parent;
    values[3] = g_roots.cwd;
    append(out, cap, &used, "\"config\":\"");
    append_escaped(out, cap, &used, g_config);
    append(out, cap, &used, "\",\"roots\":{");
    for (i = 0; i < 4; ++i) {
        if (i) append(out, cap, &used, ",");
        append(out, cap, &used, "\"%s\":\"", names[i]);
        append_escaped(out, cap, &used, values[i]);
        append(out, cap, &used, "\"");
    }
    append(out, cap, &used, "},\"paths\":[");
    for (i = 0; i < g_count; ++i) {
        const ResolvedPathRow* row = &g_rows[i];
        if (i) append(out, cap, &used, ",");
        append(out, cap, &used, "{\"key\":\"");
        append_escaped(out, cap, &used, row->key);
        append(out, cap, &used, "\",\"configured\":\"");
        append_escaped(out, cap, &used, row->configured);
        append(out, cap, &used, "\",\"resolved\":\"");
        append_escaped(out, cap, &used, row->resolved);
        append(out, cap, &used, "\",\"exists\":%s,\"source\":\"",
               row->exists ? "true" : "false");
        append_escaped(out, cap, &used, row->source);
        append(out, cap, &used, "\"}");
    }
    append(out, cap, &used, "]");
    if (used >= cap) {
        out[0] = '\0';
        return 0;
    }
    return used;
}
