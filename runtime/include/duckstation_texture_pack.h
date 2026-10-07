#ifndef PSXRECOMP_DUCKSTATION_TEXTURE_PACK_H
#define PSXRECOMP_DUCKSTATION_TEXTURE_PACK_H

#include "hd_texture_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DuckTexturePack DuckTexturePack;

enum DuckTextureKind { DUCK_TEXTURE_UPLOAD = 0, DUCK_TEXTURE_PAGE = 1 };

/* Independent representation of the modern filename wire format. Source
 * width is in VRAM words; offsets and rectangle sizes are expanded texels.
 * Hashes are unseeded XXH3-64 over explicitly little-endian native words. */
typedef struct DuckTextureKey {
    uint64_t source_hash;
    uint64_t palette_hash;
    uint16_t source_width_words;
    uint16_t source_height;
    uint16_t offset_x;
    uint16_t offset_y;
    uint16_t width;
    uint16_t height;
    uint8_t kind;
    uint8_t depth; /* enum HdTextureDepth */
    uint8_t semitransparent; /* STP4 / STP8 / STC16 alpha convention */
    uint8_t palette_min;
    uint8_t palette_max;
} DuckTextureKey;

typedef struct DuckTexturePackInfo {
    const char* root;
    const char* diagnostic; /* unsupported config/formats and malformed names */
    size_t replacement_count;
    size_t ambiguous_count;
    size_t ignored_count;
} DuckTexturePackInfo;

typedef struct DuckTextureMatch {
    DuckTextureKey key;
    uint64_t entry_id;
    const char* replacement_path;
    /* Origin of replacement rectangle relative to current texture page, in
     * texels. May be negative for uploads spanning more than one page. */
    int32_t origin_u;
    int32_t origin_v;
    uint16_t source_width;
    uint16_t source_height;
} DuckTextureMatch;

typedef struct DuckTexturePixels {
    const uint8_t* rgba;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    void* lease;
} DuckTexturePixels;

/* Accepts a game/package folder containing replacements/ and dumps/, or the
 * replacements directory itself. Missing folders are valid empty packs, so
 * a new pack can start by dumping. Recursive PNG scan; no game assets bundled.
 * Non-PNG payloads, unsupported options and invalid names are diagnosed. */
int duck_texture_pack_create(const char* root, DuckTexturePack** out_pack,
                             char* error, size_t error_capacity);
void duck_texture_pack_destroy(DuckTexturePack* pack);
void duck_texture_pack_get_info(const DuckTexturePack* pack,
                                DuckTexturePackInfo* out_info);

int duck_texture_parse_name(const char* filename, DuckTextureKey* out_key);
int duck_texture_format_name(const DuckTextureKey* key, char* output,
                             size_t capacity); /* stem only, without .png */
uint64_t duck_texture_hash_words_le(const uint16_t* words, size_t count);
/* Native word rectangle must not wrap. Zero on invalid input (also a valid
 * hash value); use valid geometry rather than zero as a success indication. */
uint64_t duck_texture_hash_rect(const uint16_t* vram, size_t count,
                                uint16_t x, uint16_t y,
                                uint16_t width_words, uint16_t height);

/* Track post-write native VRAM, after PSX set/check-mask semantics. Wrapped
 * uploads are invalidated but deliberately not tracked. Copies, coalescing,
 * and split-upload identities are outside this initial compatibility subset.
 * Any touched upload is dropped, so partial overwrites fail conservatively. */
int duck_texture_pack_track_upload(DuckTexturePack* pack,
                                   uint16_t x, uint16_t y,
                                   uint16_t width_words, uint16_t height,
                                   const uint16_t* vram, size_t count);
void duck_texture_pack_invalidate(DuckTexturePack* pack,
                                  uint16_t x, uint16_t y,
                                  uint16_t width_words, uint16_t height);
void duck_texture_pack_reset_tracking(DuckTexturePack* pack);
/* Same-session reload only: copy immutable upload identities while the caller
 * keeps native VRAM unchanged. Lookup, image and dump caches stay independent.
 * This is not a savestate restoration API. Strong replacement on success. */
int duck_texture_pack_copy_tracking(DuckTexturePack* destination,
                                    const DuckTexturePack* source);

/* Only one replacement covering the entire inclusive query is accepted.
 * UV wrapping, edge-wrapped pages and multi-image composition fall back.
 * Uses HD_TEXTURE_LOOKUP_* statuses. Query geometry is shared with Beetle. */
int duck_texture_pack_match(DuckTexturePack* pack,
                            const HdTextureDrawQuery* query,
                            DuckTextureMatch* out_match);
/* Prefer ST names on semitransparent primitives and ordinary names otherwise;
 * when only the other convention exists it remains usable. The filename's
 * convention always determines how the renderer interprets its alpha. */
int duck_texture_pack_match_draw(DuckTexturePack* pack,
                                 const HdTextureDrawQuery* query,
                                 int semitransparent, DuckTextureMatch* out_match);

/* One bounded background worker; no PNG decoding in the render thread.
 * Pixels retain raw DuckStation alpha bytes. The renderer interprets the
 * key.semitransparent flag; this is not conventional opacity for ST names.
 * Release the lease even after pack destruction. */
void duck_texture_pack_set_decode_budget(DuckTexturePack* pack, size_t bytes);
int duck_texture_pack_request_decode(DuckTexturePack* pack, uint64_t entry_id);
int duck_texture_pack_acquire_decoded(DuckTexturePack* pack, uint64_t entry_id,
                                     DuckTexturePixels* out_pixels);
void duck_texture_pixels_release(DuckTexturePixels* pixels);

/* Dumps the word-aligned draw rectangle. Uses texupload when exactly one
 * unchanged tracked upload covers it; otherwise texpage. Full palette range
 * is emitted; transparent native zero stays zero, native STP uses alpha 128
 * for ST names. Existing files are never overwritten. Writes native decoded
 * pixels only, never already-replaced pixels. Returns FOUND for a new dump,
 * NONE for an existing/skipped dump, ERROR on invalid input or write failure.
 * FOUND means queued; bounded background writes finish on pack destruction.
 * Any asynchronous write failure is reported by the next dump call. */
int duck_texture_pack_dump_draw(DuckTexturePack* pack,
                                const HdTextureDrawQuery* query,
                                int semitransparent,
                                char* error, size_t error_capacity);
void duck_texture_pack_reset_dump(DuckTexturePack* pack);

#ifdef __cplusplus
}
#endif
#endif
