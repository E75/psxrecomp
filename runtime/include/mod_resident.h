#pragma once
/*
 * Resident disc resources for title-owned seamless-loading adapters.
 *
 * A seamless loader serves a game's blocking loads from memory instead of the
 * emulated drive, with the game's own code still consuming every byte. The
 * title-independent half lives here: read the listed files from the effective
 * (mod-patched) disc once, optionally derive data from them with the title's
 * own decoders, verify everything with SHA-256, and keep a cache that is
 * reused only by the exact same mod plan. The adapter keeps its loader hooks,
 * its catalog and its decoders.
 *
 * Cache key: SHA-256 over the active mod plan fingerprint, the adapter's
 * format id and the complete spec (paths, stock sizes/hashes, policy). Every
 * blob is re-verified when a cache is loaded; a mismatch rebuilds it. Packs
 * live in LOCALAPPDATA (or XDG_CACHE_HOME / ~/.cache)/<title>/seamless, or in
 * PSX_RESIDENT_CACHE when set. Only files named <format>-*.pack are pruned.
 *
 * Call psx_resident_prepare() from an activation callback (it reads the disc
 * through psx_mod_read_disc_file). Accessors are emulation-thread only.
 * TCP: {"cmd":"resident_status"} lists every prepared pack.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Serve mod-effective content. Files that differ from their stock hash
     * are counted (psx_resident_file_stock() == 0), never replaced. */
    PSX_RESIDENT_ALLOW_MODIFIED = 0,
    /* Fail preparation when any file with a stock hash differs: for adapters
     * whose hooks depend on the original layout. The title then loads
     * normally. */
    PSX_RESIDENT_REQUIRE_STOCK = 1
};

typedef struct PSXResidentFile {
    const char* path;          /* ISO 9660 path, e.g. "CD/TOMBA2.IMG" */
    uint32_t stock_size;       /* original byte size; 0 = not catalogued */
    const char* stock_sha256;  /* lowercase hex over the zero-padded sectors of
                                * the original file; NULL = not catalogued */
} PSXResidentFile;

typedef struct PSXResidentSink PSXResidentSink;

/* Store one derived blob (decoded asset, table, ...) for source file `file`.
 * `tag` and `meta` are the adapter's own; blobs are kept in emission order and
 * identical bytes are stored once. Returns 0 on failure (preparation fails). */
int psx_resident_emit(PSXResidentSink* sink, uint32_t file, uint32_t tag,
                      const uint32_t meta[4], const void* bytes, uint32_t size);

/* Called once per file during preparation, in spec order, with the file's
 * sector-padded effective content. `stock` is 1 when it matched its stock
 * hash. Return 0 to fail preparation (e.g. a stock decode mismatch). */
typedef int (*PSXResidentDeriveFn)(PSXResidentSink* sink, uint32_t file,
                                   const uint8_t* data, uint32_t size,
                                   int stock, void* user);

typedef struct PSXResidentSpec {
    uint32_t struct_size;            /* sizeof(PSXResidentSpec) */
    const char* title;               /* cache folder, e.g. "MegaManX6Recomp" */
    const char* format;              /* pack id; change it whenever the derived
                                      * data's meaning changes */
    const PSXResidentFile* files;
    uint32_t file_count;
    uint32_t policy;                 /* PSX_RESIDENT_ALLOW_MODIFIED / _REQUIRE_STOCK */
    uint32_t max_file_bytes;         /* 0 = a full 80-minute CD */
    PSXResidentDeriveFn derive;      /* optional */
    void* derive_user;
    uint32_t keep_packs;             /* packs of this format kept besides the
                                      * active one; 0 = 3 */
} PSXResidentSpec;

typedef struct PSXResidentPack PSXResidentPack;

/* Load (verified) or build the pack. NULL on failure; the reason is kept for
 * resident_status and the resident.prepare_failed mod counter is bumped. A
 * later prepare for the same title and format releases the earlier pack. */
const PSXResidentPack* psx_resident_prepare(const PSXResidentSpec* spec);

uint32_t psx_resident_file_count(const PSXResidentPack* pack);
/* Sector-padded effective bytes. `size` receives the file size, `padded` the
 * padded length (both optional). NULL for an invalid index. */
const uint8_t* psx_resident_file(const PSXResidentPack* pack, uint32_t file,
                                 uint32_t* size, uint32_t* padded);
/* Effective-disc LBA of the file's first sector (relocations by mods apply). */
uint32_t psx_resident_file_lba(const PSXResidentPack* pack, uint32_t file);
int psx_resident_file_stock(const PSXResidentPack* pack, uint32_t file);
uint32_t psx_resident_modified_files(const PSXResidentPack* pack);
/* `bytes` starting at effective LBA `lba`, entirely within one file's padded
 * content; NULL otherwise. `file` (optional) receives the file index. */
const uint8_t* psx_resident_find_lba(const PSXResidentPack* pack, uint32_t lba,
                                     uint32_t bytes, uint32_t* file);

uint32_t psx_resident_derived_count(const PSXResidentPack* pack);
/* Derived blob `index` in emission order. Optional out-params. */
const uint8_t* psx_resident_derived(const PSXResidentPack* pack, uint32_t index,
                                    uint32_t* file, uint32_t* tag,
                                    uint32_t meta[4], uint32_t* size);

/* Lowercase hex SHA-256 of the guest bytes in [lo, hi) of each range, hashed
 * in order, compared with `sha256`. Guards an adapter's code contract. */
typedef struct PSXResidentRange { uint32_t lo, hi; } PSXResidentRange;
int psx_resident_guest_ranges_match(const PSXResidentRange* ranges,
                                    uint32_t count, const char* sha256);

/* Debug server: JSON array describing every prepared pack. */
int psx_resident_status_json(char* out, uint32_t capacity);

#ifdef __cplusplus
}
#endif
