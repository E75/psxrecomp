#ifndef PSXRECOMP_GPU_HD_TEXTURES_H
#define PSXRECOMP_GPU_HD_TEXTURES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emulation-thread configuration. Roots are external user-owned directories;
 * a successful replacement session selects native CPU VRAM authority on GL.
 * Reconfiguration retains the previous session if opening the new root fails.
 * Empty root with both options off clears the current session. */
int gpu_hd_textures_configure(const char* root, int replacements_enabled,
                             int dump_enabled, char* error, size_t capacity);
void gpu_hd_textures_shutdown(void);
int gpu_hd_textures_reload(char* error, size_t capacity);
void gpu_hd_textures_set_dump_enabled(int enabled);
/* Park the session (CPU raster authority and pack lookups off) and bring it
 * back later from the same root. Meant for screens that need no replacement. */
void gpu_hd_textures_set_paused(int paused);
int gpu_hd_textures_replacements_enabled(void);
int gpu_hd_textures_dump_enabled(void);
int gpu_hd_textures_active(void);
typedef struct GpuHdTextureDiag {
    const char* root;
    int active, replacements, dump, format;
    uint64_t draw_queries, matched_draws, ready_draws, applied_draws, dumped_textures;
    size_t replacement_count;
    const char* diagnostic;
    uint64_t pending_dump_sources;
    /* Backend-neutral "a replacement was turned into a sampled texture" and
     * "pixels it contains", so OpenGL and Vulkan report the same numbers. */
    uint64_t materialized_textures, pixels_replaced;
} GpuHdTextureDiag;
void gpu_hd_textures_get_diag(GpuHdTextureDiag* out);
void gpu_hd_textures_note_applied(void);
/* Called once per replacement image a backend actually uploads/samples. */
void gpu_hd_textures_note_texture(uint32_t width, uint32_t height);

/* Why a packed page did not match. Filled for the first few distinct
 * (page, depth) misses of a session so a live run can prove whether the key
 * scheme, the palette or the upload residency is the reason. Hashes use the
 * pack's own derivation: CRC-32 over the VRAM words, low byte then high. */
#define GPU_HD_MISS_DIAG_MAX 8
#define GPU_HD_PROBE_MAX 64      /* distinct pages hashed for the diagnostic */
typedef struct GpuHdTextureMissDiag {
    uint16_t page_x, page_y;
    uint8_t  depth;                 /* 0=4bpp 1=8bpp 2=16bpp */
    uint16_t clut_x, clut_y;
    uint8_t  u_first, u_last, v_first, v_last;
    uint32_t page_hash;             /* CRC of the whole page rect (what the
                                     * generator writes for a one-shot upload) */
    uint32_t footprint_hash;        /* CRC of the rect this prim samples */
    uint32_t palette_hash;          /* CRC of the live CLUT */
    uint32_t nearest_texture_hash;  /* closest pack texture key to page_hash */
    uint32_t nearest_palette_hash;
    int      nearest_prefix_bits;   /* common leading bits, 32 = identical */
    int      page_key_known;        /* page_hash is a pack texture key */
    int      footprint_key_known;   /* footprint_hash is a pack texture key */
    int      palette_key_known;     /* palette_hash is a pack palette key */
    uint64_t draws;                 /* misses recorded for this page */
} GpuHdTextureMissDiag;
int gpu_hd_textures_miss_diag(GpuHdTextureMissDiag* out, int capacity);
/* Non-GL backends can observe native draws for dumping; replacements remain
 * an OpenGL presentation feature. */
void gpu_hd_textures_observe_draw(uint16_t texpage, uint16_t clut_x,
                                 uint16_t clut_y, const int limits[4],
                                 uint32_t texture_window, int semitransparent);

/* Native VRAM and residency hooks. A full restage resets identity rather than
 * pretending a savestate is an original texture upload. */
void gpu_hd_textures_set_vram(const uint16_t* vram);
void gpu_hd_textures_track_upload(int x, int y, int width_words, int height,
                                 const uint16_t* words);
/* Header-before-payload invalidation and pre/post native copy observations. */
void gpu_hd_textures_begin_upload(int x, int y, int width_words, int height);
void gpu_hd_textures_begin_copy(int source_x, int source_y, int destination_x,
                                int destination_y, int width_words, int height);
void gpu_hd_textures_end_copy(void);
void gpu_hd_textures_invalidate(int x, int y, int width_words, int height);
void gpu_hd_textures_reset_tracking(void);

typedef struct GpuHdTextureImage {
    const uint8_t* rgba;
    uint32_t width, height, stride;
    uint64_t cache_key;
    uint64_t generation;
    int32_t origin_u, origin_v;
    uint32_t source_width, source_height;
    /* 1: Beetle (native STP), 2: Duck non-ST, 3: Duck ST alpha encoding,
     * 4: composed cutout/STP/opaque classes (alpha 0/128/255). */
    int alpha_mode;
    void* lease;
} GpuHdTextureImage;

/* Inclusive page-UV bounds, before native raster writes the destination.
 * Texture-window masks are applied to the queried footprint. Pending decode,
 * ambiguity and unsupported geometry return native sampling. Ready partial
 * replacements compose with native holes; pending parts never decode here. */
int gpu_hd_textures_acquire_draw(uint16_t texpage, uint16_t clut_x,
                                uint16_t clut_y, const int limits[4],
                                uint32_t texture_window, int semitransparent,
                                GpuHdTextureImage* image);
void gpu_hd_textures_release_image(GpuHdTextureImage* image);

#ifdef __cplusplus
}
#endif
#endif
