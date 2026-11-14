/* vram_ext.h - host-side VRAM extension for displays wider than 512 px.
 *
 * A guest framebuffer wider than 512 pixels necessarily covers VRAM columns
 * 512..W-1, where this game keeps texture pages. The texture layout is data
 * (asset pack + baked tpage fields), so it cannot be re-ordered, and the total
 * budget does not fit either. The PS1's VRAM is the limit; ours is not.
 *
 * The guest-visible VRAM stays EXACTLY 1024x512 and every address keeps its
 * meaning (texture pages, CLUTs, uploads, savestates, hashes). The host keeps
 * one extra plane for the part of the framebuffer that falls past column 512:
 *
 *   guest column 512..703  ->  extension column 0..191
 *   host column = guest x + VRAM_EXT_OFFSET   (512 -> 1024 .. 681 -> 1193)
 *
 * so the backends' raw VRAM mirror is VRAM_EXT_HOST_W = 1216 texels wide:
 * guest columns 0..1023, then the extension at 1024..1215. Texture sampling
 * never sees the extension (guest coordinates are masked to 0..1023).
 *
 * Only primitive DRAW output is redirected. CPU->VRAM uploads (GP0 0xA0/0xC0,
 * DMA, MDEC) always write the guest plane: they carry textures, and redirecting
 * them would relocate exactly the data this feature protects.
 *
 * Enabled by [video] vram_extension = true (default false). Env override:
 * PSX_VRAM_EXTENSION=1/0. With it off every path is byte-identical to before.
 */
#ifndef PSX_VRAM_EXT_H
#define PSX_VRAM_EXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRAM_EXT_GUEST_W 1024   /* guest VRAM width - never changes */
#define VRAM_EXT_X0      512    /* first guest column redirected */
#define VRAM_EXT_COLS    192    /* extension plane width (guest 512..703) */
#define VRAM_EXT_HOST_W  (VRAM_EXT_GUEST_W + VRAM_EXT_COLS)  /* 1216 */
#define VRAM_EXT_OFFSET  512    /* host x = guest x + VRAM_EXT_OFFSET */

typedef enum {
    VRAM_EXT_BACKEND_NONE     = 0,
    VRAM_EXT_BACKEND_SOFTWARE = 1,
    VRAM_EXT_BACKEND_OPENGL   = 2,
    VRAM_EXT_BACKEND_VULKAN   = 3
} VramExtBackend;

/* Parse the config once (idempotent). Returns 1 when the feature is enabled. */
int  vram_ext_init(void);
int  vram_ext_enabled(void);

/* Which backend is driving VRAM. Only OpenGL and Vulkan write the extension
 * plane; the software rasterizer does not (see docs/VRAM_EXTENSION.md). */
void vram_ext_set_backend(int backend);
int  vram_ext_backend(void);
/* enabled && the active backend writes/reads the extension plane. */
int  vram_ext_driving(void);

/* The framebuffer rect whose overflow columns are redirected, in guest
 * coordinates. gpu.c sets it whenever the draw area or the display geometry
 * changes; on == 0 means "this frame draws somewhere else" (render-to-texture,
 * 4:3 display, depth24, display off) and nothing is redirected. */
void vram_ext_set_region(int on, int x0, int y0, int x1, int y1);
int  vram_ext_region(int *x0, int *y0, int *x1, int *y1);

/* Per-pixel rule: extension-plane index (row * VRAM_EXT_COLS + col) for a
 * guest pixel that belongs in the extension, else -1. */
int  vram_ext_plane_index(int x, int y);
/* Host column for guest x at row y (x + VRAM_EXT_OFFSET when redirected). */
int  vram_ext_map_x(int x, int y);

/* The host extension plane: VRAM_EXT_COLS x 512 halfwords, column 0 == guest
 * column VRAM_EXT_X0. Filled by the backend readback, read by the CPU present
 * and by guest GP0 0xC0 reads of the framebuffer. */
uint16_t *vram_ext_host(void);

/* Counters (debug server "gpu_state" -> "vram_ext_stats"). */
void vram_ext_note_remap(uint64_t pixels);
void vram_ext_note_readback(uint64_t bytes);
void vram_ext_note_present(int width);
/* A wide display whose frame is drawn with the gate OFF (draw area is not the
 * framebuffer: render-to-texture). Diagnostic: must stay 0 for WE2002. */
void vram_ext_note_gate_off(uint64_t frames);
void vram_ext_reset_stats(void);
int  vram_ext_debug_json(char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_VRAM_EXT_H */
