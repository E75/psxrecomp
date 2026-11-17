#ifndef PSX_GPU_VK_RENDERER_H
#define PSX_GPU_VK_RENDERER_H

/* gpu_vk_renderer.c — hardware Vulkan renderer backend.
 *
 * Selected via [video] renderer = "vulkan".  Like the GL backend it is
 * GPU-authoritative: VRAM lives as a Vulkan image and present blits straight
 * from it to the swapchain (no per-frame readback).  Vulkan is loaded ENTIRELY
 * at runtime through SDL_Vulkan_LoadLibrary + vkGetInstanceProcAddr, so the exe
 * has no link-time vulkan-1 dependency and gracefully falls back to software on
 * a machine without a Vulkan ICD.
 *
 * The backend supplies its vtable through vk_backend_get() (NULL until a context
 * is created and the pipeline is ready). */

struct SDL_Window;

/* Compile-time ceiling for the Vulkan internal-resolution scale. The render
 * target covers only the displayed window (VRAM_W x VRAM_H is the fallback),
 * so the 4K target is ~512x480 native x S instead of 1024x512 x S. 16 keeps
 * 8K (S=18 on a 240-line title) reachable through the "match display" preset
 * once the window is small enough; the real limit is the device's
 * maxImageDimension2D, applied at target creation. */
#define VK_MAX_INTERNAL_SCALE 16

#ifdef __cplusplus
extern "C" {
#endif

/* Create the Vulkan instance/device/swapchain on a window made with
 * SDL_WINDOW_VULKAN.  Returns 1 on success, 0 to fall back to software. */
int  vk_renderer_init_context(struct SDL_Window *win);

/* Tear down all Vulkan objects (idempotent). */
void vk_renderer_shutdown(void);

/* Present the displayed VRAM region straight from the GPU VRAM image to the
 * swapchain (15-bit frames; deterministic, no readback). Returns 1 on success,
 * 0 if the Vulkan path is inactive (caller keeps the CPU present path). */
int  vk_renderer_present_vram(int disp_x, int disp_y, int w, int h,
                              int linear, int force_4_3);

/* GPU-direct native-wide present (mirrors gl_renderer_present_wide_fbo). */
int  vk_renderer_present_wide(int disp_x, int disp_y, int disp_h, int linear);

/* Present an ARGB8888 image as a letterboxed quad (24-bit FMV frames and the
 * display-disabled clear). force_4_3 pillarboxes at native 4:3. */
void vk_renderer_present_cpu(const uint32_t *pixels, int src_w, int src_h,
                             int linear, int force_4_3);
void vk_renderer_present_blank(void);

/* Sync the GPU VRAM image down into the CPU VRAM mirror (screenshots, debug
 * server, 24-bit present). No-op when the Vulkan path is inactive. */
void vk_renderer_sync_cpu(void);

/* After savestate restore: push CPU VRAM into the Vulkan image (see GL twin). */
void vk_renderer_restage_vram_after_savestate(void);

/* Set the present policy: 1=tear-free, 0=IMMEDIATE (lowest latency, may tear),
 * -1=MAILBOX. Tear-free prefers MAILBOX because the frontend already paces
 * frames; unsupported modes fall back to FIFO (always available). */
void vk_renderer_set_present_mode(int mode);
void vk_renderer_set_display_aspect(int num, int den);

/* Temporal 60 fps presenter: blend each pair of the guest's flips into two
 * presents (the Vulkan twin of gl_renderer_set_interpolation). Presentation
 * only - guest logic, timers and audio keep their own cadence. */
void vk_renderer_set_interpolation(int enabled, double host_hz, double target_hz,
                                   double source_hz, int blend_mode);
void vk_renderer_set_interpolation_blend(int blend_mode);
void vk_renderer_set_interpolation_suspended(int suspended);
void vk_renderer_set_interpolation_source(int source);
/* 1 while this presenter owns the frame deadline (main.cpp's pacer stands
 * down, exactly like gl_renderer_interpolation_owns_cadence). */
int  vk_renderer_interpolation_owns_cadence(void);

/* Per-frame GPU/CPU timing + op counters (debug server "vk_frame_perf"): a JSON
 * array of up to `count` samples, oldest..newest, each with the frame index,
 * CPU frame time, GPU scene/present time from timestamp queries, allocs,
 * submits and draw calls. */
int  vk_frame_perf_json(char *out, int cap, int count);

/* HD texture pack changed (opened/reloaded/closed from the debug server or the
 * env config). Drops materialized overrides and re-applies the CLUT-row
 * overrides to the GPU VRAM mirror. Safe to call when no pack is loaded. */
void vk_renderer_hd_pack_changed(void);

/* Always-on present ring (debug server "vk_present_ring"), the Vulkan twin of
 * gl_present_ring: EVERY swap the presenter performs, including interpolation
 * sub-presents. This is the backend-neutral pacing reading (t_us diffs). */
enum { VK_PRES_VRAM = 0, VK_PRES_WIDE = 1, VK_PRES_CPU = 2,
       VK_PRES_BLANK = 3, VK_PRES_INTERP = 4 };
typedef struct VkPresEvent {
    uint32_t frame;      /* guest present index the swap belongs to */
    uint8_t  path;       /* VK_PRES_* */
    uint32_t t_ms;
    uint64_t t_us;       /* monotonic microseconds at the swap */
    int dx, dy, w, h;    /* source rect (native VRAM px) */
    int lx, ly, lw, lh;  /* letterbox rect (window px) */
} VkPresEvent;
uint64_t vk_renderer_pres_total(void);
int      vk_renderer_pres_get(uint64_t seq, VkPresEvent *out);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_VK_RENDERER_H */
