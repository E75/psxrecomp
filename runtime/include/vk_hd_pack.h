#ifndef PSX_VK_HD_PACK_H
#define PSX_VK_HD_PACK_H

#include <stddef.h>
#include <stdint.h>

#include "gpu_hd_textures.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vulkan HD texture-pack surface.
 *
 * The pack itself is the SAME session the OpenGL backend uses
 * (gpu_hd_textures_*): one directory, one key scheme, one set of hashes. This
 * module adds the two things Vulkan needs on top of it:
 *   1. a query/release shim that counts what the backend actually applied, and
 *   2. the CLUT-row override case: `<texture>-<palette>.clut.png` files are
 *      resolved to a palette hash and rewritten into the GPU-side VRAM copy at
 *      upload time (a 16-entry 5-5-5 palette replacement). */

typedef struct VkHdPackStats {
    int loaded;                /* a pack is open and replacements are enabled */
    uint64_t overrides;        /* distinct textures materialized on the GPU */
    uint64_t pixels_replaced;  /* RGBA pixels copied into override images */
    uint64_t misses;           /* draw queries that fell back to native art */
    uint64_t palettes;         /* 16-entry CLUT blocks rewritten in VRAM */
    uint64_t clut_files;       /* `.clut.png` overrides found in the pack */
    size_t entries;            /* replacement PNGs in the pack */
    const char* root;          /* pack root (owned by the session) */
} VkHdPackStats;

/* Open the pack named by PSXRECOMP_HD_PACK (fallback PSXRECOMP_HD_TEXTURE_ROOT)
 * once. A pack already configured by the mods/launcher path is left alone.
 * Returns 1 when replacements are active. */
int  vk_hd_pack_init(void);

/* Explicit root (debug server / config). root == NULL or "" closes the session.
 * Re-scans the CLUT overrides. Returns 1 on success, 0 with `error` filled. */
int  vk_hd_pack_set_root(const char* root, char* error, size_t capacity);

int  vk_hd_pack_active(void);
void vk_hd_pack_get_stats(VkHdPackStats* out);

/* One draw query against the shared pack session. On success *out describes an
 * RGBA image owned by the caller; always release it with vk_hd_pack_release. */
int  vk_hd_pack_acquire(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                        const int limits[4], uint32_t window, int semi,
                        GpuHdTextureImage* out);
void vk_hd_pack_release(GpuHdTextureImage* image);

/* Counters for the backend: one materialized override of w x h pixels. */
void vk_hd_pack_note_texture(uint32_t width, uint32_t height);

/* Upload-time CLUT patch. `words` is the w x h block being written to the GPU
 * VRAM mirror (row-major, x/y in VRAM words). Every fully covered 16-word
 * aligned block whose palette hash has an override is rewritten in place.
 * Returns the number of 16-entry palettes rewritten. */
int  vk_hd_pack_patch_clut(uint16_t* words, int x, int y, int w, int h);

/* Palette-hash lookup used by the boot sweep (full VRAM, no upload pending).
 * Returns 1 and fills out[16] when an override exists for these 16 entries. */
int  vk_hd_pack_clut_lookup(const uint16_t* palette16, uint16_t out[16]);

#ifdef __cplusplus
}
#endif

#endif /* PSX_VK_HD_PACK_H */
