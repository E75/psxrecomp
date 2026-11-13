/*
 * resolved_paths.h — the `resolved_paths` debug command and the startup log.
 *
 * Every asset/config key the runtime resolves is recorded once, in the order it
 * was resolved, with the configured value, the absolute resolved path and
 * whether that path exists. docs/ASSET_PATHS.md is the reference; the acceptance
 * harness reads this command so a live asset row can only FAIL on a genuine
 * absence instead of on a guess about the process working directory.
 *
 * Read-only: it reports what the resolvers already decided, it never creates,
 * rewrites or probes a game state.
 */

#ifndef PSX_RESOLVED_PATHS_H
#define PSX_RESOLVED_PATHS_H

#include "host_path_resolve.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the game.toml path and the four rule anchors (docs/ASSET_PATHS.md).
 * main.cpp calls this once at startup; the report echoes them so a reader can
 * see which anchors produced the answers. */
void psx_resolved_paths_set_roots(const char* config_path,
                                  const PsxPathRoots* roots);

/* Record one key. The first note for a key wins (resolution is once per key at
 * startup) and is logged once; `source` is "config", "env", "default" or "cli".
 * Implemented in resolved_paths.c. */
void psx_resolved_paths_note(const char* key, const char* configured,
                             const char* resolved, const char* source);

/* The resolved value already recorded for `key`, or NULL. */
const char* psx_resolved_paths_get(const char* key);

/* JSON body for the `resolved_paths` command:
 * "config":"<game.toml>","roots":{...},"paths":[{"key":..,"configured":..,
 * "resolved":..,"exists":true,"source":..},...]. Returns the length, 0 on a
 * buffer that cannot hold it. */
int psx_resolved_paths_json(char* out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_RESOLVED_PATHS_H */
