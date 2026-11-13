#ifndef PSXRECOMP_ASSET_PROVIDER_H
#define PSXRECOMP_ASSET_PROVIDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * asset_provider -- native disc sector source for the recompiled runtime.
 *
 * Serves the 2048-byte user data of a disc sector from the extracted tree
 * (<disc_dir>/manifest.json + <disc_dir>/<path>) instead of the mounted image,
 * so a load does not go through ISOReader, 2352-byte reads or XA
 * classification. It is a PURE SECTOR SOURCE: cdrom.c asks it first and keeps
 * its existing path for every sector it does not cover. A missing manifest,
 * a disabled provider or a sector outside the manifest all return 0 and the
 * emulated drive answers exactly as before.
 *
 * Streamed content is never covered (always falls through to the ISO):
 *   - CD-DA / XA / STR extents: *.DA, *.XA, *.RA, *.STR, and anything under
 *     MOVIE/ or SD/DA/ (the extracted copies are lossy or absent);
 *   - manifest entries whose status is not "ok" (extraction was short);
 *   - the trailing partial sector of a file (only whole 2048-byte sectors).
 */

#define ASSET_PROVIDER_SECTOR_SIZE 2048

typedef struct {
    int      enabled;          /* 1 = manifest loaded, sectors may be served */
    int      files;            /* manifest entries usable as a sector source */
    int      preloaded_files;  /* entries held entirely in RAM */
    int      frontend_files;   /* front-end (menu) packs inside the preload */
    uint64_t preloaded_bytes;  /* RAM held by the preload */
    uint64_t hits;             /* sectors answered from the extracted tree */
    uint64_t misses;           /* lookups not covered -> caller used the ISO */
    uint64_t bytes;            /* 2048-byte payloads served */
    char     disc_dir[512];    /* resolved directory (empty when disabled) */
    char     error[256];       /* why it is disabled (empty when enabled) */
} AssetProviderStats;

/*
 * disc_dir: directory holding manifest.json + the extracted files.
 * preload: copy whole files into RAM at init (up to preload_max_mib).
 * preload_max_mib: RAM budget; 0 = no preload even when preload is set.
 * excludes / exclude_count: path prefixes kept out of RAM (preload only).
 * Returns 1 when the manifest was loaded, 0 otherwise (provider disabled).
 */
int  asset_provider_init(const char* disc_dir, int preload, int preload_max_mib,
                         const char* const* excludes, int exclude_count);

/* Free the preload. Safe to call when never initialized. */
void asset_provider_shutdown(void);

/*
 * Copy the 2048-byte user data of `lba` into `dst`.
 * Returns 1 when served, 0 when the sector is not covered (caller falls
 * through to the emulated drive). Never writes `dst` on 0.
 */
int asset_provider_read_sector(uint32_t lba, uint8_t* dst);

/* Snapshot the counters/config for the debug server. */
void asset_provider_get_stats(AssetProviderStats* out);

/* Zero hits/misses/bytes (keeps the loaded manifest and the preload). */
void asset_provider_reset_stats(void);

/*
 * Patched guest asset images (docs/ASSET_OVERRIDE.md).
 *
 * A directory holds manifest.json (format "we2002-asset-override/1") plus the
 * patched files. Each entry names a guest destination, a byte budget and the
 * retail disc read it replaces:
 *
 *   files[]: { path, guest_address, bytes, sha256,
 *              max_bytes?, ceiling?,
 *              retail_load?: { lba, sectors, dest } }
 *
 * Apply rule, two parts, no game-code change:
 *   (a) sectors [lba, lba+sectors) are answered from the patched file's first
 *       sectors*2048 bytes instead of the disc/tree (asset_provider_read_sector);
 *   (b) the remaining tail is copied once into guest RAM at
 *       guest_address + sectors*2048 when that last sector has been drained
 *       (asset_provider_note_read_complete, called by cdrom.c).
 *
 * Every validation failure (missing manifest, bad sha256, size over
 * max_bytes/ceiling, dest != guest_address) drops that entry and leaves the
 * stock path byte-identical: the entry is never served and never written.
 */
typedef struct {
    int      enabled;        /* override manifest loaded */
    int      entries;        /* usable entries */
    int      applied;        /* entries whose head+tail are in guest RAM */
    int      skipped;        /* entries refused at load (see reason) */
    uint64_t bytes;          /* patched bytes served or written */
    char     override_dir[512];
    char     reason[256];    /* first refusal reason (empty when none) */
} AssetOverrideStats;

/* Load <dir>/manifest.json. Returns 1 when at least one entry is usable.
 * Independent of the extracted-tree provider above: an override may be the
 * only source (it then supplies every sector of its retail read). */
int  asset_provider_override_init(const char* dir);
void asset_provider_override_shutdown(void);

/* Called by cdrom.c when the sector buffer of `lba` is fully consumed, i.e.
 * the guest has the retail read. Writes the pending tail once. Cheap when no
 * override is loaded (single branch). */
void asset_provider_note_read_complete(uint32_t lba);

void asset_provider_get_override_stats(AssetOverrideStats* out);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_ASSET_PROVIDER_H */
