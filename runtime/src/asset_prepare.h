/* In-exe asset preparation (docs/IN_EXE_ASSETS.md).
 *
 * On first start, with the user's own disc image, produce the artefacts that
 * are cheap to derive in C: the raw 2352-byte STR extent behind the PSTR
 * header that audio_native.c reads. The HD texture pack and the MODEL.BIN
 * override still need the Python tools; this module only reports them.
 *
 * Nothing here overwrites an existing artefact and nothing is fatal: a
 * missing/invalid disc leaves the game booting exactly as before.
 */
#ifndef PSX_ASSET_PREPARE_H
#define PSX_ASSET_PREPARE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASSET_PREPARE_PATH_MAX 1024

typedef struct AssetPrepareOptions {
    const char* disc_path; /* cue or bin of the user's disc; NULL/"" = none */
    const char* root_dir;  /* directory holding assets/ (the game.toml dir) */
    int         force;     /* --prepare-assets: write the report even if complete */
} AssetPrepareOptions;

typedef struct AssetPrepareResult {
    int  prepared;       /* artefacts written this run */
    int  present;        /* artefacts already on disk (never overwritten) */
    int  missing_python; /* artefacts that still need the Python tools */
    char str_raw_path[ASSET_PREPARE_PATH_MAX];
    char report_path[ASSET_PREPARE_PATH_MAX];
} AssetPrepareResult;

/* 0 = nothing to do / done, >0 = artefacts prepared, -1 = could not run.
 * Never aborts the game. A short reason is available via last_error(). */
int asset_prepare_run(const AssetPrepareOptions* opt, AssetPrepareResult* out);

/* Reason for the last negative return ("" when there is none). */
const char* asset_prepare_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_ASSET_PREPARE_H */
