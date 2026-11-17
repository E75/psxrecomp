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
#define VRAM_EXT_HOST_X0 (VRAM_EXT_X0 + VRAM_EXT_OFFSET)     /* 1024: first plane column */

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
 * changes; on == 0 means "the draw that is arriving now is not the
 * framebuffer" (render-to-texture, 4:3 display, depth24, display off).
 *
 * The GEOMETRY is sticky: a gate that closes for one command (the match
 * re-asserts its display env every flip, and the register arm widens the clip
 * in a second write) must not make the pack forget which rows/columns are the
 * wide framebuffer, or the very next pack writes the framebuffer back into the
 * guest texture pages. So `on == 1` refreshes the geometry, `on == 0` only
 * closes the draw gate, and the geometry survives like the learned bands until
 * a GPU reset (vram_ext_reset_bands clears both). */
void vram_ext_set_region(int on, int x0, int y0, int x1, int y1);
void vram_ext_clear_region(void);
/* The framebuffer geometry (sticky): what the pack redirects and what the
 * display/readback paths read from the plane. */
int  vram_ext_region(int *x0, int *y0, int *x1, int *y1);
/* The live per-draw gate: 1 only while the draw area currently being set up IS
 * the wide display buffer. Draw-time redirect decisions (a CPU-rasterized
 * fill, a VRAM copy) use this; the pack must use vram_ext_region(). */
int  vram_ext_draw_gate(int *x0, int *y0, int *x1, int *y1);

/* Framebuffer row bands. A double-buffered title draws one 240-line band per
 * frame and scans out one of them, and the pack of a frame's draws can run
 * AFTER the game flipped to the other band - so the redirect must cover every
 * band ever used as a display buffer, not just the one being drawn now.
 * gpu.c learns them (draw area X span == display X span, band height == the
 * display height) and forgets them when the display geometry changes. */
void vram_ext_learn_band(int y0, int y1);
void vram_ext_reset_bands(void);
int  vram_ext_band_rows(int *y0, int *y1);   /* union of the learned bands */

/* Per-pixel rule: extension-plane index (row * VRAM_EXT_COLS + col) for a
 * guest pixel that belongs in the extension, else -1. Uses the sticky
 * framebuffer geometry (a plane pixel is the frame, not a draw in progress). */
int  vram_ext_plane_index(int x, int y);
/* Host column for guest x at row y (x + VRAM_EXT_OFFSET when redirected). */
int  vram_ext_map_x(int x, int y);
/* The host extension plane: VRAM_EXT_COLS x 512 halfwords, column 0 == guest
 * column VRAM_EXT_X0. Filled by the backend readback, read by the CPU present
 * and by guest GP0 0xC0 reads of the framebuffer. */
uint16_t *vram_ext_host(void);

/* Counters (debug server "gpu_state" -> "vram_ext_stats"). */
/* One pack rect, in guest coordinates: counts the framebuffer pixels that went
 * to the extension plane (present-side) and - the assertion - any framebuffer
 * column that went into GUEST VRAM instead (must be 0; >0 means the pack wrote
 * over a texture page). */
void vram_ext_note_pack_rect(int x0, int y0, int x1, int y1);
/* A CPU-rasterizer draw pixel redirected into the extension plane (software
 * backend / a GL fallback frame). Counted separately from the pack. */
void vram_ext_note_cpu_redirect(uint64_t pixels);
/* One pack pass (a draw/dispatch) and its fragment count, for the cost check. */
void vram_ext_note_pack_passes(uint64_t passes, uint64_t pixels);
/* CPU wall time a backend spent inside one pack call (ms). The pack is the
 * guest->plane copy: the render target's band rows into guest VRAM (columns
 * 0..511), the extension plane (512..681) and the past-edge texture columns. */
void vram_ext_note_pack_time(double ms);
/* GL only: a frame where the GPU raster was bypassed (CPU rasterizer drew, the
 * GPU present no-opped). reason: 1 = no context, 2 = raster not ok, 3 = depth24
 * skip, 4 = dual-raster authority, 5 = HD native authority, 6 = frame-gen draw. */
void vram_ext_note_gl_bypass(int reason);
/* A texture fetch must never resolve through the redirect: pass 1 if a texture
 * or upload path resolved a coordinate through vram_ext_map_x(). Every texture
 * path in the tree passes the canonical guest plane, so this stays 0 and
 * `texture_reads_ok` proves it. */
void vram_ext_note_texture_read(int remapped);
void vram_ext_note_present(int width);
void vram_ext_note_readback(uint64_t bytes);
/* A wide display whose frame is drawn with the gate OFF (draw area is not the
 * framebuffer: render-to-texture). Diagnostic: must stay 0 for WE2002. */
void vram_ext_note_gate_off(uint64_t frames);
void vram_ext_reset_stats(void);
int  vram_ext_debug_json(char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_VRAM_EXT_H */
