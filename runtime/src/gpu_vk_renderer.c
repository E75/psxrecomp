/* gpu_vk_renderer.c — hardware Vulkan renderer backend.  See gpu_vk_renderer.h.
 *
 * GPU-authoritative VRAM (mirrors the GL backend's model): the canonical copy of
 * VRAM is the native R16 image (s_raw_img, 1024x512) that textured draws sample
 * and readback copies from.  The RGBA8 *render target* (s_vram_img) only holds
 * the displayed window at scale S (see tgt_create), and a CPU mirror is kept
 * for readback (screenshots / debug server / 24-bit FMV present), synced
 * lazily.  Present blits the displayed region straight from the target to the
 * swapchain — no per-frame readback.
 *
 * Vulkan is loaded entirely at runtime (SDL_Vulkan_LoadLibrary +
 * vkGetInstanceProcAddr), so the exe never link-depends on vulkan-1 and falls
 * back to software when no ICD is present.
 *
 * ---- PHASE PLAN / BUILD STATUS -----------------------------------------
 * The port from gpu_gl_renderer.c is COMPLETE. Nothing in this file aborts
 * via psx_fatal_halt; the plan below is the order the phases were built in
 * and doubles as the map for anyone touching one of them.
 *
 *   phase 0  context      runtime loader, instance, device, swapchain,
 *                         per-frame sync — create_instance..create_sync   done
 *   phase 1  targets      RGBA8 hr VRAM image + D24S8/D32S8 (stencil bit0
 *                         mirrors VRAM bit15) + pack compute mirror       done
 *   phase 2  geometry     batched flat/gouraud triangles, flat rects,
 *                         lines as quads; GEO program, dynamic viewport,
 *                         scissor, no dithering (col555 /31.0f)          done
 *   phase 3  texturing    TEX program: raw R16 VRAM mirror sampled with
 *                         texelFetch, 4/8-bit CLUT, texture window,
 *                         per-vertex modulation, gpu_uv.h limits          done
 *   phase 4  masking      stencil SET / CHECK-keep / CHECK-invert as
 *                         pipeline cache keys (bind_masked*)              done
 *   phase 5  semi-transp  PS1 modes 0..3 as Vulkan blend states (constant
 *                         alpha 1/2, 1/4) + STP two-pass split            done
 *   phase 6  SSAA         s_scale 1..4 on every target; present reads the
 *                         hr image directly (no downscale blit)           done
 *   phase 7  native-wide  per-base_x mirror surfaces with their own
 *                         depth-stencil + framebuffers, mirrored in the
 *                         SAME command buffer as the canonical pass       done
 *   phase 8  present      opaque present shader pass, FMV CPU present,
 *                         OSD/rewind/savestate overlays, wide present     done
 *
 * Known gap: the optional vtable member wide_dump_full (debug server
 * "wide_full" PNG dump) has no Vulkan twin — gr_wide_dump_full() returns 0
 * here while software/OpenGL implement it. It is a diagnostic-only readback,
 * not part of the per-frame path. Keep the rule: implement it, or leave the
 * entry NULL / abort — never add a silent no-op stub.
 * ------------------------------------------------------------------------ */

#include "gpu.h"
#include "gpu_render.h"
#include "gpu_vk_renderer.h"
#include "gpu_sw_renderer.h"
#include "host_osd.h"
#include "psx_savestate_menu.h"
#include "psx_rewind.h"
#include "crash_trace.h"
#include "gpu_vk_upload.h"
#include "gpu_hd_textures.h"
#include "vk_hd_pack.h"
#include "latency_ring.h"
#include "frame_interpolation.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#if !defined(PSX_HAVE_VULKAN)
/* Vulkan SDK headers/glslc were not found at configure time: compile an inert
 * backend so the runtime still builds and simply falls back to software. */
int  vk_renderer_init_context(struct SDL_Window *win) { (void)win; return 0; }
void vk_renderer_shutdown(void) {}
int  vk_renderer_present_vram(int a,int b,int c,int d,int e,int f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}
int  vk_renderer_present_wide(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;return 0;}
void vk_renderer_present_cpu(const uint32_t*p,int w,int h,int l,int f){(void)p;(void)w;(void)h;(void)l;(void)f;}
void vk_renderer_present_blank(void){}
void vk_renderer_sync_cpu(void){}
void vk_renderer_restage_vram_after_savestate(void){}
void vk_renderer_set_present_mode(int m){(void)m;}
void vk_renderer_set_display_aspect(int n,int d){(void)n;(void)d;}
int  vk_perf_json(char *out,int cap,int count){(void)count; return cap>2?snprintf(out,cap,"[]"):0;}
int  vk_frame_perf_json(char *out,int cap,int count){(void)count; return cap>2?snprintf(out,cap,"{}"):0;}
uint64_t vk_renderer_pres_total(void) { return 0; }
int  vk_renderer_pres_get(uint64_t seq, VkPresEvent *out) { (void)seq; (void)out; return 0; }
void vk_renderer_hd_pack_changed(void) {}
const GpuRenderBackend *vk_backend_get(void) { return 0; }

#else  /* PSX_HAVE_VULKAN */

#include "psx_sdl.h"
#if defined(PSX_SDL3)
#include <SDL3/SDL_vulkan.h>
#else
#include <SDL_vulkan.h>
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "png_write.h"        /* png_write_rgb — present_shot readback */
#include "vk_shaders_spv.h"   /* generated: spv_geo_vert/frag, spv_geo_tex_vert/frag,
                               *            spv_pack_comp, spv_blit_vert/frag */
#include "vram_ext.h"
#include "vfx.h"              /* [video] volumetrics / light shafts (docs/EFFECTS.md) */
#include "gpu.h"              /* gpu_ot_rank: ordering-table depth proxy for fog */

#define VRAM_W 1024
#define VRAM_H 512
/* Raw-mirror width: guest VRAM (0..1023) plus the [video] vram_extension
 * columns at 1024..1215 (host x = guest x + VRAM_EXT_OFFSET for x >= 512). */
#define VRAM_HOST_W VRAM_EXT_HOST_W

static int s_aspect_num = 4, s_aspect_den = 3;

void vk_renderer_set_display_aspect(int num, int den) {
    if (num <= 0 || den <= 0) { num = 4; den = 3; }
    if (num != s_aspect_num || den != s_aspect_den)
        fprintf(stdout, "psxrecomp: vulkan: present aspect %d:%d\n", num, den);
    s_aspect_num = num; s_aspect_den = den;
}

/* ---- [video] volumetrics / light shafts (docs/EFFECTS.md) ---------------
 * Both effects are armed once, here, and every cost they add is behind these
 * flags: with volumetrics = "off" (the default) no pass, barrier, pipeline or
 * counter differs from the pre-effect renderer.
 *
 * fog     - the geometry pass writes the per-primitive distance proxy into
 *           clip-space z, which the unused depth attachment stores; the
 *           present pass reads it and mixes toward the fog colour. Guest VRAM
 *           is never modified.
 * shafts  - the present draw goes into an offscreen post image and a radial
 *           blur of its bright pixels is added on the way to the swapchain.
 */
static int   s_vfx_fog;                  /* fog present + depth proxy armed */
static int   s_vfx_shafts;               /* light-shaft pass armed          */
static float s_vfx_fog_density = 0.35f;
static float s_vfx_fog_color[3] = { 0.722f, 0.784f, 0.847f };
static float s_vfx_shaft_strength = 0.5f;
static int   s_vfx_shaft_samples = 24;
static float s_vfx_light_uv[2] = { 0.5f, 0.18f };
static uint32_t s_vfx_ot_max = 1;        /* previous frame's highest OT rank */
static uint32_t s_vfx_ot_cur;            /* this frame's highest OT rank      */
static int   s_vfx_warned_budget;
static int   s_vfx_failed;               /* resource creation failed (log once) */
static int   s_vfx_depth_cleared;        /* fog proxy cleared for this frame   */
static VkImageLayout s_vfx_ds_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
/* Depth-aspect-only view of the canonical target's DS image: a combined
 * depth/stencil view cannot be sampled, and the fog present reads the depth
 * aspect as its distance proxy. Created with the target, only when fog is
 * armed. */
static VkImageView s_ds_sample_view;

/* Present-chain resources (defined with the present code below; declared here
 * so the teardown path can release them). All NULL until first armed use. */
typedef struct { float src_rect[4]; float fog[4]; } VfxPush;   /* 32 bytes */
static VkRenderPass          s_vfx_post_rp;
static VkImage               s_vfx_post_img;
static VkDeviceMemory        s_vfx_post_mem;
static VkImageView           s_vfx_post_view;
static VkFramebuffer         s_vfx_post_fb;
static uint32_t              s_vfx_post_w, s_vfx_post_h;
static VkSampler             s_vfx_samp;
static VkDescriptorSetLayout s_vfx_fog_dsl;
static VkDescriptorSetLayout s_vfx_shaft_dsl;
static VkPipelineLayout      s_vfx_fog_pl;
static VkPipelineLayout      s_vfx_shaft_pl;
static VkPipeline            s_vfx_fog_pipe;
static VkPipeline            s_vfx_shaft_pipe;
static VkDescriptorPool      s_vfx_pool;
static VkShaderModule        s_vfx_fog_fs, s_vfx_shaft_fs;
static void vfx_release_post(void);

/* Depth proxy for the primitive being appended, or -1.0 when the effect is
 * off. PGXP's normalized 1/z (a_q) is the real thing and is used per vertex
 * inside geo_tex.vert; this is the fallback for prims without projection
 * provenance: the packet's ordering-table node index, normalized by the
 * previous frame's deepest node. The first drawn packet is the farthest (the
 * PSY-Q linked-list walk runs back to front), hence 1 - rank/max. A packet
 * with no linked list (0xFFFF, the direct/2D path) never fades. */
static float vfx_prim_fog(void) {
    if (!s_vfx_fog) return -1.0f;
    uint32_t rank = gpu_ot_rank();
    if (rank == 0xFFFFu) return 0.0f;
    if (rank > s_vfx_ot_cur) s_vfx_ot_cur = rank;
    if (s_vfx_ot_max < 4) return 0.0f;   /* not a depth-sorted list: no fade */
    float f = 1.0f - (float)rank / (float)s_vfx_ot_max;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

/* ---- dynamic loader ---------------------------------------------------- */
/* vkGetInstanceProcAddr comes from SDL; everything else is loaded through it
 * (global/instance scope) or vkGetDeviceProcAddr (device scope). */
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
static PFN_vkGetDeviceProcAddr   p_vkGetDeviceProcAddr;

#define VK_GLOBAL_FUNCS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)

#define VK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateDevice) \
    X(vkDestroySurfaceKHR)

#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkQueueWaitIdle) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) \
    X(vkQueueSubmit) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdBlitImage) \
    X(vkCmdCopyImage) \
    X(vkCmdWriteTimestamp) \
    X(vkCmdResetQueryPool) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkGetQueryPoolResults) \
    X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdSetStencilReference) \
    X(vkCmdDispatch) \
    X(vkCmdDraw) \
    X(vkCmdClearAttachments)

#define DECL_FP(n) static PFN_##n p_##n;
VK_GLOBAL_FUNCS(DECL_FP)
VK_INSTANCE_FUNCS(DECL_FP)
VK_DEVICE_FUNCS(DECL_FP)
#undef DECL_FP

/* ---- state ------------------------------------------------------------- */
static SDL_Window      *s_win;
static VkInstance       s_instance;
static VkSurfaceKHR     s_surface;
static VkPhysicalDevice s_phys;
static VkDevice         s_dev;
static uint32_t         s_qfam = 0;       /* graphics+present queue family */
static VkQueue          s_queue;
static VkCommandPool    s_cmd_pool;       /* present command buffers */
static VkCommandPool    s_work_pool;      /* one-shot work CBs (reset at gpu_sync) */
static int              s_work_pending;   /* submitted work not yet waited */
static VkPhysicalDeviceMemoryProperties s_memprops;
/* Device limits used by the perf timestamps and the windowed target. */
static float s_ts_period_ns = 1.0f;
static int   s_ts_ok;
static int   s_max_image_dim = 16384;

/* Staging buffers whose GPU consumption is still in flight; freed at gpu_sync
 * after the queue goes idle (work is submitted without per-op waits now). */
#define PENDING_STAGING_MAX 1024
static VkBuffer       s_pending_buf[PENDING_STAGING_MAX];
static VkDeviceMemory s_pending_mem[PENDING_STAGING_MAX];
static int            s_pending_n;

#define STAGING_CACHE_MAX 16
typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    void *map;
    int busy;
} StagingCacheEntry;
static StagingCacheEntry s_staging_cache[STAGING_CACHE_MAX];

static void staging_release(VkBuffer buf, VkDeviceMemory mem) {
    for (int i = 0; i < STAGING_CACHE_MAX; ++i) {
        if (s_staging_cache[i].buf == buf && s_staging_cache[i].mem == mem) {
            s_staging_cache[i].busy = 0;
            return;
        }
    }
    p_vkUnmapMemory(s_dev, mem);
    p_vkDestroyBuffer(s_dev, buf, NULL);
    p_vkFreeMemory(s_dev, mem, NULL);
}

static void staging_destroy(VkBuffer buf, VkDeviceMemory mem) {
    for (int i = 0; i < STAGING_CACHE_MAX; ++i) {
        StagingCacheEntry *entry = &s_staging_cache[i];
        if (entry->buf == buf && entry->mem == mem) {
            if (entry->map) p_vkUnmapMemory(s_dev, mem);
            p_vkDestroyBuffer(s_dev, buf, NULL);
            p_vkFreeMemory(s_dev, mem, NULL);
            memset(entry, 0, sizeof *entry);
            return;
        }
    }
    p_vkUnmapMemory(s_dev, mem);
    p_vkDestroyBuffer(s_dev, buf, NULL);
    p_vkFreeMemory(s_dev, mem, NULL);
}

static VkSwapchainKHR   s_swapchain;
static VkFormat         s_sc_format;
static VkExtent2D       s_sc_extent;
static uint32_t         s_sc_count;
static VkImage          s_sc_images[8];
static int              s_present_mode_req = 1;   /* 1 tear-free, 0 IMMEDIATE, -1 MAILBOX */

/* Per-frame sync (double-buffered command recording). */
#define VK_FRAMES 3      /* in-flight frames; 2 stalled the emu thread on the
                          * second 60 fps sub-present (each present waits the
                          * frame fence), so the temporal presenter could not
                          * sustain two presents per guest interval */
static VkCommandBuffer  s_cmd[VK_FRAMES];
static VkSemaphore      s_sem_acquire[VK_FRAMES];
static VkSemaphore      s_sem_render[VK_FRAMES];
static VkFence          s_fence[VK_FRAMES];
static uint32_t         s_frame_idx;

/* GPU-authoritative VRAM image + CPU mirror. */
static int        s_scale = 1;
/* Windowed render target: native VRAM rect actually allocated (s_tw x s_th at
 * s_scale), the presented area included. Starts as the draw area and grows to
 * the union of everything written and displayed; falls back to the whole
 * VRAM (0,0,1024,512) when the union is not worth shrinking. */
static int        s_tx0, s_ty0, s_tw = VRAM_W, s_th = VRAM_H;
static int        s_tgt_grows;           /* reallocations (diagnostic) */
static float      s_tgt_pct = 1.0f;      /* allocated / full-VRAM area */
static int        s_tgt_shrinks;         /* shrink-backs (diagnostic) */
static uint32_t   s_tgt_last_change;     /* present index of the last resize */
static VkImage    s_vram_img;
static VkDeviceMemory s_vram_mem;
static VkImageLayout s_vram_layout = VK_IMAGE_LAYOUT_UNDEFINED;  /* tracked */
static uint16_t  *s_vram;                 /* gpu.c's CPU VRAM array (1024x512) */
static int        s_cpu_dirty;            /* CPU mirror stale vs GPU image */

static int        s_ready;                /* full pipeline ready (Phase 2+) */
static int        s_ctx_ok;              /* instance/device/swapchain up */

static int        s_texfilter;

/* ---- geometry / render targets ----------------------------------------- */
static VkImageView     s_vram_view;       /* RGBA8 hr render target view */
static VkRenderPass    s_rpass;           /* color (hr) + depth/stencil */
/* [video] volumetrics: same attachments, but the depth aspect is CLEARed at
 * pass start (the geometry pass writes the fog depth proxy there; a stale
 * value from the previous frame would fade pixels nothing drew this frame).
 * Render-pass compatible with s_rpass, so the cached pipelines serve both. */
static VkRenderPass    s_rpass_fog;
/* Same, but depth LOADed: used for every geometry batch after the first in a
 * frame, so a later batch's proxy does not wipe the earlier batches'. */
static VkRenderPass    s_rpass_fog_load;
static VkFramebuffer   s_fbo;

/* Native R16_UINT raw VRAM mirror: the canonical 1555 bits, sampled in the
 * textured fragment shader (CLUT decode needs the exact index bits, which the
 * lossy RGBA8 image cannot provide) and read back to the CPU mirror. Kept
 * coherent with the hr render target by the PACK compute pass. */
static VkImage         s_raw_img;
static VkDeviceMemory  s_raw_mem;
static VkImageView     s_raw_view;
static VkImageLayout   s_raw_layout = VK_IMAGE_LAYOUT_UNDEFINED;

/* Depth/stencil attachment: stencil bit0 mirrors VRAM bit15 (mask). */
static VkImage         s_ds_img;
static VkDeviceMemory  s_ds_mem;
static VkImageView     s_ds_view;
static VkFormat        s_ds_format;
/* Both candidate formats (D24S8 / D32S8) are COMBINED depth+stencil: views,
 * barriers and image clears must name BOTH aspects unless
 * separateDepthStencilLayouts is enabled (it is not). Stencil-only leaves the
 * depth half in UNDEFINED; on AMD (no D24S8 -> D32S8) depth and stencil share
 * compression metadata. */
#define DS_ASPECT (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)

/* Scratch hr image for VRAM->VRAM copies (resolves overlap). */
static VkImage         s_scratch_img;
static VkDeviceMemory  s_scratch_mem;
static VkImageView     s_scratch_view;
static VkImageLayout   s_scratch_layout = VK_IMAGE_LAYOUT_UNDEFINED;

/* Native-res RGBA8 upload staging image (CPU->VRAM blit source). */
static VkImage         s_up_img;
static VkDeviceMemory  s_up_mem;
static VkImageView     s_up_view;
static VkImageLayout   s_up_layout = VK_IMAGE_LAYOUT_UNDEFINED;

/* Samplers + descriptor sets. One nearest sampler serves every fetch. */
/* ---- native-wide compositor surfaces (mirrors the GL backend) -----------
 * Canonical VRAM (the hr image) stays 100% faithful; framebuffer-targeting
 * batches are ALSO drawn into a per-base_x wide surface, x-translated in the
 * vertex shader via the existing u_xoff/u_xhalf push constants. Each surface
 * carries its OWN depth-stencil image (PSX mask-bit mirror; Part-A lesson:
 * never render stencil-enabled passes into a stencil-less target) and its own
 * VkFramebuffer on the SHARED render pass, so every existing pipeline works
 * unchanged. The mirror is appended to the SAME one-shot command buffer as the
 * canonical pass — one extra render pass per flushed batch, no extra submits. */
#define VK_WIDE_MAX_SURF 4
static VkImage        s_wide_img[VK_WIDE_MAX_SURF];
static VkDeviceMemory s_wide_mem[VK_WIDE_MAX_SURF];
static VkImageView    s_wide_view[VK_WIDE_MAX_SURF];
static VkImage        s_wide_ds_img[VK_WIDE_MAX_SURF];
static VkDeviceMemory s_wide_ds_mem[VK_WIDE_MAX_SURF];
static VkImageView    s_wide_ds_view[VK_WIDE_MAX_SURF];
static VkFramebuffer  s_wide_fb[VK_WIDE_MAX_SURF];
static VkImageLayout  s_wide_layout[VK_WIDE_MAX_SURF];
static int            s_wide_base[VK_WIDE_MAX_SURF] = { -1, -1, -1, -1 };
static int            s_wide_w = 0;        /* wide width (native px); 0 = disabled */
static int            s_wide_offset = 0;   /* centering OFFSET (native px) */
static int            s_wide_cur = -1;     /* active mirror surface index, -1 = none */
static int            s_wide_cur_base = 0;
static void wide_free_all(void);           /* defined with the wide helpers below */

static VkSampler       s_samp;
static VkDescriptorPool s_dpool;
static VkDescriptorSetLayout s_dsl_tex;   /* binding0: raw usampler2D (frag) */
static VkDescriptorSetLayout s_dsl_pack;  /* binding0: hr sampler, binding1: raw storage (compute) */
static VkDescriptorSetLayout s_dsl_blit;  /* binding0: src sampler (frag) */
static VkDescriptorSet s_ds_tex;
static VkDescriptorSet s_ds_pack;
/* Blit src image is rebound per blit; a ring of sets avoids updating a set still
 * in flight (work is no longer waited per-op). Cycled within a frame; the index
 * resets at gpu_sync (which waits the queue idle). */
#define BLIT_DESC_RING 64
static VkDescriptorSet s_ds_blit_ring[BLIT_DESC_RING];
static int s_ds_blit_idx;

/* ---- always-on per-frame perf ring (transition-stall diagnosis) ----------
 * Per-op counters accumulate into s_perf_cur; each present() snapshots them into
 * the ring keyed by present index, then resets. Query via the debug-server
 * "vk_perf" command (vk_perf_json). Op COUNTS alone reveal the dominant
 * per-frame cost (e.g. make_staging vkAllocateMemory churn during transitions:
 * a frame that jumps from ~10 to ~2000 allocs is the smoking gun). Always on. */
typedef struct {
    uint32_t present_idx;
    uint64_t present_qpc;
    uint32_t wait_us, acquire_us, present_us;
    uint32_t allocs, alloc_kb, oneshots, submits, syncs, pack_flushes,
             blits, upload_blocks, copy_rects, geo_flushes, tex_flushes,
             wide_passes, wide_clears, draws;
    /* CPU/GPU phase timing (vk_frame_perf). cpu_ms is present-to-present;
     * scene_gpu/present_gpu come from the timestamp pair of this frame and are
     * filled in a few frames late (has_gpu until then). vfx_ms is the cost of
     * the [video] volumetrics / light-shaft passes inside the present
     * (docs/EFFECTS.md); has_vfx is 0 on frames that recorded none. */
    float    cpu_ms, scene_gpu_ms, present_gpu_ms, vfx_ms;
    uint32_t has_gpu, has_vfx;
    /* Render-target footprint + the native rects this frame wrote, so the
     * windowed-target policy is observable (tgt = allocated bounds,
     * cover = union of draws/fills/uploads/copies, oob = writes clipped
     * outside tgt). */
    int32_t  tx0, ty0, tw, th;
    int32_t  cx0, cy0, cx1, cy1;
    uint32_t oob;
} VkPerf;
#define VK_PERF_RING 256
static VkPerf s_perf_ring[VK_PERF_RING];
static uint32_t s_perf_head;          /* number of frames recorded (monotonic) */
static VkPerf s_perf_cur = { .cx1 = -1, .cy1 = -1 };   /* current frame */
static uint32_t s_present_idx;
/* Timestamp queries: 5 per frame (scene start, present start, present end,
 * vfx start, vfx end), VK_PERF_TS_SLOTS frames in flight, read VK_PERF_TS_LAG
 * frames late so the debug-server query never waits on the GPU. The two vfx
 * slots are only written on frames that record a [video] volumetrics pass. */
#define VK_PERF_TS_SLOTS 8
#define VK_PERF_TS_LAG   6
static VkQueryPool s_perf_pool;
static int         s_perf_ts_frame_done;   /* q0 written for the current frame */
static uint64_t    s_perf_last_present_qpc;

/* Extend the frame's write-coverage rect (native VRAM px) and count writes
 * clipped by the target bounds. */
static void perf_cover(int x0, int y0, int x1, int y1) {
    if (x1 < x0 || y1 < y0) return;
    if (s_perf_cur.cx1 < s_perf_cur.cx0) {          /* empty */
        s_perf_cur.cx0 = x0; s_perf_cur.cy0 = y0;
        s_perf_cur.cx1 = x1; s_perf_cur.cy1 = y1;
        return;
    }
    if (x0 < s_perf_cur.cx0) s_perf_cur.cx0 = x0;
    if (y0 < s_perf_cur.cy0) s_perf_cur.cy0 = y0;
    if (x1 > s_perf_cur.cx1) s_perf_cur.cx1 = x1;
    if (y1 > s_perf_cur.cy1) s_perf_cur.cy1 = y1;
}

static uint32_t perf_elapsed_us(uint64_t start) {
    uint64_t frequency = SDL_GetPerformanceFrequency();
    uint64_t ticks = SDL_GetPerformanceCounter() - start;
    return frequency ? (uint32_t)(ticks * 1000000u / frequency) : 0;
}

/* ---- always-on present ring (backend-neutral pacing reading) -------------
 * One record per swapchain present, including interpolation sub-presents.
 * Same event shape as gl_present_ring so tools/we/perf.py and acceptance can
 * diff t_us for both backends without a branch per field. */
#define VK_PRES_RING 4096
static VkPresEvent s_pres_ring[VK_PRES_RING];
static uint64_t    s_pres_seq;

static void pres_record(int path, int dx, int dy, int w, int h,
                        int lx, int ly, int lw, int lh) {
    static uint64_t pres_freq;
    if (!pres_freq) { pres_freq = SDL_GetPerformanceFrequency(); if (!pres_freq) pres_freq = 1; }
    VkPresEvent *e = &s_pres_ring[s_pres_seq % VK_PRES_RING];
    e->frame = (uint32_t)s_present_idx;
    e->path  = (uint8_t)path;
    e->t_ms  = (uint32_t)SDL_GetTicks();
    e->t_us  = (uint64_t)((double)SDL_GetPerformanceCounter() * 1.0e6 / (double)pres_freq);
    e->dx = dx; e->dy = dy; e->w = w; e->h = h;
    e->lx = lx; e->ly = ly; e->lw = lw; e->lh = lh;
    s_pres_seq++;
}

uint64_t vk_renderer_pres_total(void) { return s_pres_seq; }
int vk_renderer_pres_get(uint64_t seq, VkPresEvent *out) {
    if (seq >= s_pres_seq) return 0;
    const VkPresEvent *e = &s_pres_ring[seq % VK_PRES_RING];
    if (e->t_us == 0 || seq + VK_PRES_RING <= s_pres_seq) return 0;  /* overwritten */
    *out = *e;
    return 1;
}

/* Timestamp q0: first command buffer of the frame (scene start). Safe to call
 * from any recording CB; no-op once the frame already has its q0. */
static void perf_ts_scene_begin(VkCommandBuffer cb) {
    if (!s_ts_ok || s_perf_ts_frame_done) return;
    uint32_t slot = s_present_idx % VK_PERF_TS_SLOTS;
    p_vkCmdResetQueryPool(cb, s_perf_pool, slot * 5, 5);
    p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          s_perf_pool, slot * 5 + 0);
    s_perf_ts_frame_done = 1;
}

/* Read one frame's timestamp pair into its ring slot (no GPU wait). */
static void perf_try_read_gpu(uint32_t frame_idx) {
    if (!s_ts_ok || s_perf_head == 0) return;
    VkPerf *p = &s_perf_ring[frame_idx % VK_PERF_RING];
    if (p->present_idx != frame_idx || p->has_gpu) return;
    uint32_t slot = frame_idx % VK_PERF_TS_SLOTS;
    if (frame_idx + VK_PERF_TS_SLOTS <= s_present_idx) return;  /* slot reused */
    uint64_t ts[3] = {0, 0, 0};
    VkResult r = p_vkGetQueryPoolResults(s_dev, s_perf_pool, slot * 5, 3,
                                         sizeof ts, ts, sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return;   /* not ready yet */
    if (ts[2] >= ts[1] && ts[1] >= ts[0]) {
        p->scene_gpu_ms   = (float)(ts[1] - ts[0]) * s_ts_period_ns / 1.0e6f;
        p->present_gpu_ms = (float)(ts[2] - ts[1]) * s_ts_period_ns / 1.0e6f;
        p->has_gpu = 1;
    }
    /* q3/q4 exist only on frames that recorded a volumetrics pass. */
    if (p->has_vfx) {
        uint64_t vts[2] = {0, 0};
        if (p_vkGetQueryPoolResults(s_dev, s_perf_pool, slot * 5 + 3, 2,
                                    sizeof vts, vts, sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            vts[1] >= vts[0]) {
            p->vfx_ms = (float)(vts[1] - vts[0]) * s_ts_period_ns / 1.0e6f;
            vfx_report_cost(p->vfx_ms);
            /* Budget guard (docs/EFFECTS.md): the effect set is only defensible
             * while it stays inside ~1.5 ms at 1440p. Report it, once. */
            if (p->vfx_ms > 1.5f && !s_vfx_warned_budget) {
                s_vfx_warned_budget = 1;
                fprintf(stdout, "psxrecomp: vfx: pass cost %.2f ms exceeds the "
                                "1.5 ms budget (fog=%d shafts=%d taps=%d)\n",
                        (double)p->vfx_ms, s_vfx_fog, s_vfx_shafts, s_vfx_shaft_samples);
            }
        }
    }
}

/* Read the timestamps of frames that are far enough behind to be complete.
 * Called from the present snapshot (one frame per frame, VK_PERF_TS_LAG late)
 * and from the debug-server command; never blocks. */
static void perf_collect_gpu(void) {
    if (!s_ts_ok) return;
    uint32_t first = s_present_idx > VK_PERF_TS_SLOTS - 1
                   ? s_present_idx - (VK_PERF_TS_SLOTS - 1) : 0;
    for (uint32_t f = first; f < s_present_idx; f++) perf_try_read_gpu(f);
    if (s_present_idx > VK_PERF_TS_LAG)
        perf_try_read_gpu(s_present_idx - VK_PERF_TS_LAG);
}

static void perf_snapshot_present(void) {
    s_perf_cur.present_idx = s_present_idx++;
    s_perf_cur.present_qpc = SDL_GetPerformanceCounter();
    /* [video] volumetrics: the ordering-table proxy is normalized by the
     * deepest node of the PREVIOUS frame, because the current frame's total is
     * only known here. */
    if (s_vfx_fog) {
        if (s_vfx_ot_cur > s_vfx_ot_max) s_vfx_ot_max = s_vfx_ot_cur;
        s_vfx_ot_cur = 0;
        s_vfx_depth_cleared = 0;   /* next frame's first geometry pass clears */
    }
    if (s_perf_last_present_qpc) {
        uint64_t freq = SDL_GetPerformanceFrequency();
        s_perf_cur.cpu_ms = freq
            ? (float)((double)(s_perf_cur.present_qpc - s_perf_last_present_qpc)
                      * 1000.0 / (double)freq)
            : 0.0f;
    }
    s_perf_last_present_qpc = s_perf_cur.present_qpc;
    s_perf_cur.tx0 = s_tx0; s_perf_cur.ty0 = s_ty0;
    s_perf_cur.tw = s_tw;  s_perf_cur.th = s_th;
    s_perf_ring[s_perf_head % VK_PERF_RING] = s_perf_cur;
    s_perf_head++;
    memset(&s_perf_cur, 0, sizeof s_perf_cur);
    s_perf_cur.cx0 = 0; s_perf_cur.cy0 = 0; s_perf_cur.cx1 = -1; s_perf_cur.cy1 = -1;
    s_perf_ts_frame_done = 0;
    /* Patch the GPU time of the frame VK_PERF_TS_LAG presents ago: its query
     * has completed by now, and reading it here never stalls the queue. */
    if (s_present_idx > VK_PERF_TS_LAG)
        perf_try_read_gpu(s_present_idx - 1 - VK_PERF_TS_LAG);
}

/* Pipeline layouts (one per program family). */
static VkPipelineLayout s_pl_geo;
static VkPipelineLayout s_pl_tex;
static VkPipelineLayout s_pl_blit;
static VkPipelineLayout s_pl_pack;
static VkPipeline       s_pipe_pack;      /* compute */

/* Lazy graphics-pipeline cache keyed by (program, topology, blend, stencil,
 * cmask). program: 0 GEO, 1 TEX, 2 BLIT. blend: 0 off, 1..4 = PS1 modes 0..3
 * (+1). stencil: 0 SET (always+replace, dyn ref), 1 CHECK keep, 2 CHECK
 * invert. cmask: 0 writes colour, 1 disables ALL colour writes (stencil-only
 * fixup pass of an opaque textured batch — mirrors GL's glColorMask off). */
#define PIPE_PROGS 3
#define PIPE_TOPOS 2   /* 0 TRIANGLE_LIST, 1 LINE_LIST */
#define PIPE_BLENDS 5
#define PIPE_STENCILS 3
#define PIPE_CMASKS 2
#define PIPE_FOGS 2    /* 0 no depth write, 1 depth-proxy write (volumetrics) */
#define PIPE_CACHE_N (PIPE_PROGS * PIPE_TOPOS * PIPE_BLENDS * PIPE_STENCILS * PIPE_CMASKS * PIPE_FOGS)
static VkPipeline s_pipe_cache[PIPE_CACHE_N];
static VkShaderModule s_mod_geo_v, s_mod_geo_f, s_mod_tex_v, s_mod_tex_f, s_mod_blit_v, s_mod_blit_f;

/* Untextured batch (flat/gouraud triangles, lines-as-quads, flat rects). */
typedef struct { float x, y, r, g, b, a; } Vert;   /* GEO vertex, stride 24 */
#define VK_VBUF_VERTS 262144            /* one frame's untextured verts fit: no
                                         * mid-frame overflow wait now that the
                                         * per-present sync is gone */
static VkBuffer       s_vbuf;
static VkDeviceMemory s_vbuf_mem;
static Vert          *s_vmap;             /* persistently mapped host-visible */
static uint32_t       s_vcount;           /* verts accumulated this batch */
/* Draws are SUBMITTED without waiting (deferred-submit model), so a flushed
 * batch's vertices may still be read by the GPU when the next batch begins.
 * Rewriting the buffer from offset 0 per batch raced that read and shredded
 * whole batches on screen (terrain/mesh chunks missing or drawn with another
 * batch's vertices). Batches therefore SUB-ALLOCATE: each flush draws
 * [base, base+count) and bumps base; gpu_sync (queue idle) recycles to 0. */
static uint32_t       s_vbase;            /* first vertex of the open batch */
static int            s_geo_semi = -2;    /* open untextured batch keys */
static int            s_geo_mask = 0, s_geo_check = 0;
/* Fog depth proxy of the OPEN batch ([video] volumetrics): the packet's
 * ordering-table rank, captured when the batch opened. -1 = effect off. */
static float          s_geo_fog = -1.0f;
static int            s_geo_mirror_suppress = 0;   /* open batch: skip the wide mirror
                                                    * (full-screen overlay rect draws its
                                                    * own full-width wide pass instead) */

/* Sub-pixel vertex override + perspective UV weights for the next triangle
 * ([video] geometry_correction / perspective_texturing; see gpu_render.h).
 * gpu.c sets these immediately before the matching gr_draw_*_triangle and they
 * are consumed by it. All-zero == the faithful integer/affine path. */
static int   s_pc_valid = 0;           /* sub-pixel positions present */
static float s_pc_x[3], s_pc_y[3];     /* native VRAM px, fractional  */
static int   s_pq_valid = 0;           /* perspective weights present */
static float s_pq[3];

/* Textured batch: 24 floats/vert (pos,uv,col,tpage,clut,depth,raw,limits,q,
 * hd_source,hd_mode). q is the perspective weight ([video]
 * perspective_texturing); 0 = affine, which the vertex shader turns into
 * w == 1.0 — the faithful default. hd_mode == 0 is the native sampling fast
 * path; the HD pack only touches the last five floats of an overridden prim. */
#define TEXV 24
typedef struct { float v[TEXV]; } TexVert;
#define VK_TBUF_VERTS 98304               /* multiple of 3; see VK_VBUF_VERTS */
static VkBuffer       s_tbuf;
static VkDeviceMemory s_tbuf_mem;
static TexVert       *s_tmap;
static uint32_t       s_tcount;
static uint32_t       s_tbase;            /* first vertex of the open batch */
static int            s_tb_semi = -2, s_tb_mask = 0, s_tb_check = 0, s_tb_filter = 0;
static int            s_tb_twin[4] = {0,0,0,0};
static float          s_tb_fog = -1.0f;   /* open batch fog proxy (see s_geo_fog) */

/* ---- HD texture-pack override cache ------------------------------------
 * Overrides are materialized once per texture identity (pack key + pack
 * generation) into an RGBA8 image and reused for every draw of that texture.
 * The pixels themselves arrive from the upload-time pack match
 * (gpu_hd_textures_acquire_draw -> hd_texture_pack_match), so nothing is
 * decoded or re-uploaded per draw; a prim without an override keeps the raw
 * VRAM path bit-for-bit. */
#define VK_HD_CACHE_MAX 64
#define VK_HD_CACHE_BUDGET (96u * 1024u * 1024u)
typedef struct {
    VkImage         img;
    VkDeviceMemory  mem;
    VkImageView     view;
    VkDescriptorSet set;
    VkImageLayout   layout;
    uint64_t        key, generation;
    uint32_t        w, h;
    size_t          bytes;
    uint64_t        last_use;
} HdVkTexture;
static HdVkTexture  s_hd_cache[VK_HD_CACHE_MAX];
static size_t       s_hd_cache_bytes;
static uint64_t     s_hd_cache_clock;
static VkDescriptorPool s_hd_dpool;
static VkImage        s_hd_dummy_img;
static VkDeviceMemory s_hd_dummy_mem;
static VkImageView    s_hd_dummy_view;
static VkDescriptorSet s_tb_hd_set;       /* descriptor set of the open batch */


static void flush_geometry(void);         /* commit pending untextured batch */
static void flush_tex_batch(void);        /* commit pending textured batch */
static void flush_cpu_upload(void);       /* pending CPU writes -> GPU images */
static void pack_flush(void);             /* hr -> raw mirror (dirty rect) */
static void flush_pack_if_sampling(int tpx, int tpy, int depth, int clx, int cly);
static void vram_upload_block(int x, int y, int w, int h, const uint16_t *data);
static void gpu_copy_rect(int sx, int sy, int dx, int dy, int w, int h);
static void vkb_draw_flat_rect(int x, int y, int w, int h, uint16_t c);
static void ensure_cpu(void);             /* GPU -> CPU mirror readback */
static int  make_staging(VkDeviceSize bytes, VkBuffer *buf, VkDeviceMemory *mem, void **map);
static void free_staging(VkBuffer buf, VkDeviceMemory mem);
static void hd_write_set(VkDescriptorSet set, VkImageView hd_view);
static void hd_cache_clear(void);

/* draw area / offset / per-prim render state (set via the vtable setters) */
static int s_da_x1, s_da_y1, s_da_x2 = VRAM_W - 1, s_da_y2 = VRAM_H - 1;
static int s_off_x, s_off_y;
static int s_semi_en = 0, s_semi_mode = 0;
static int s_mask_set = 0, s_mask_check = 0;
static int s_tw_mask_x = 0, s_tw_mask_y = 0, s_tw_off_x = 0, s_tw_off_y = 0;
static int s_mod_r = 128, s_mod_g = 128, s_mod_b = 128, s_mod_raw = 0;

/* Coherency: regions rendered to the hr image but not yet packed to the raw
 * mirror, and whether the hr image diverges from the CPU mirror. */
typedef struct { int set, x0, y0, x1, y1; } DirtyRect;
static DirtyRect s_pack_dirty;
static int       s_gpu_dirty;
/* CPU-side VRAM writes (GP0 A0 transfers, DMA, fills, single pokes) land in
 * the CPU array immediately and accumulate here; ONE batched upload flushes
 * them before the next GPU op that could observe them (mirrors the GL
 * backend). Eager per-op uploads are the transition-stall:
 * each PIXEL cost 2 vkAllocateMemory + 2 vkQueueSubmit + a 2-draw blit pass,
 * so a single GP0 A0 texture upload wedged the main thread for minutes inside
 * the driver (freeze dump: 8/8 samples in D3DKMTCreateAllocation churn).
 *
 * EXACT rect list, NOT a single union (ported from the GL fix for the MMX6
 * black-frame flicker, ISSUES #7): a union of DISJOINT uploads covers the
 * framebuffers in between, and flushing paints that whole box from the CPU
 * VRAM array — STALE under VK once the hr image is authoritative (ensure_cpu
 * syncs on demand only) — stomping freshly rendered frames. Only pixels the
 * CPU actually wrote may ever be uploaded. See up_add()/up_add_transfer(). */
/* 64 (vs GL's 16): VK pays TWO queue submits per flush (copies + blit pass),
 * so overflow-triggered flushes must stay rare. MDEC/FMV macroblock streams
 * coalesce into one rect per block row via the row-band merge, so a whole
 * streamed frame fits comfortably. */
#define UP_RECTS_MAX 64
static DirtyRect s_up_rects[UP_RECTS_MAX];
static int       s_up_nrects = 0;

static void rect_clear(DirtyRect *r) { r->set = 0; }
static void rect_add(DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (x0 > x1 || y0 > y1) return;
    if (!r->set) { r->x0=x0; r->y0=y0; r->x1=x1; r->y1=y1; r->set=1; return; }
    if (x0 < r->x0) r->x0 = x0; if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1; if (y1 > r->y1) r->y1 = y1;
}
static int rect_intersects(const DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (!r->set) return 0;
    return !(x1 < r->x0 || x0 > r->x1 || y1 < r->y0 || y0 > r->y1);
}

/* Add an uploaded rect (see s_up_rects). Merges ONLY when the merge adds no
 * uncovered pixels: containment either way, or same row-band / column-band
 * extension. On overflow: flush the pending set (order-preserving) when the
 * device is up; pre-ready the CPU array is fully authoritative, so a union is
 * harmless there. Mirrors the GL backend's up_add (20k-randomized-rect
 * host-test-proven merge rule). */
static void up_add(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int i = 0; i < s_up_nrects; i++) {
        DirtyRect *r = &s_up_rects[i];
        if (x0 >= r->x0 && x1 <= r->x1 && y0 >= r->y0 && y1 <= r->y1)
            return;                                   /* contained */
        if (x0 <= r->x0 && x1 >= r->x1 && y0 <= r->y0 && y1 >= r->y1) {
            r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1;  /* contains */
            return;
        }
        if (y0 == r->y0 && y1 == r->y1 &&
            x0 <= r->x1 + 1 && x1 >= r->x0 - 1) {     /* same row-band extension */
            if (x0 < r->x0) r->x0 = x0;
            if (x1 > r->x1) r->x1 = x1;
            return;
        }
        if (x0 == r->x0 && x1 == r->x1 &&
            y0 <= r->y1 + 1 && y1 >= r->y0 - 1) {     /* same column-band extension */
            if (y0 < r->y0) r->y0 = y0;
            if (y1 > r->y1) r->y1 = y1;
            return;
        }
    }
    if (s_up_nrects >= UP_RECTS_MAX) {
        if (s_ready) {
            flush_cpu_upload();       /* order-preserving: land the old ones */
        } else {
            DirtyRect *r = &s_up_rects[0];
            for (int i = 1; i < s_up_nrects; i++) {
                if (s_up_rects[i].x0 < r->x0) r->x0 = s_up_rects[i].x0;
                if (s_up_rects[i].y0 < r->y0) r->y0 = s_up_rects[i].y0;
                if (s_up_rects[i].x1 > r->x1) r->x1 = s_up_rects[i].x1;
                if (s_up_rects[i].y1 > r->y1) r->y1 = s_up_rects[i].y1;
            }
            if (x0 < r->x0) r->x0 = x0;
            if (y0 < r->y0) r->y0 = y0;
            if (x1 > r->x1) r->x1 = x1;
            if (y1 > r->y1) r->y1 = y1;
            s_up_nrects = 1;
            return;
        }
    }
    DirtyRect *r = &s_up_rects[s_up_nrects++];
    r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1; r->set = 1;
}

/* GP0(A0) transfer: per-pixel wrap means a wrapping transfer touches up to
 * FOUR exact rects — never all of VRAM (the "wrapped: take all" union was the
 * same stale-stomp class). Mirrors the GL backend's up_add_transfer. */
static void up_add_transfer(int x, int y, int w, int h) {
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    if (w <= 0 || h <= 0) return;
    int w1 = w, w2 = 0, h1 = h, h2 = 0;
    if (x + w > VRAM_W) { w1 = VRAM_W - x; w2 = w - w1; }
    if (y + h > VRAM_H) { h1 = VRAM_H - y; h2 = h - h1; }
    up_add(x, y, x + w1 - 1, y + h1 - 1);
    if (w2)       up_add(0, y, w2 - 1, y + h1 - 1);
    if (h2)       up_add(x, 0, x + w1 - 1, h2 - 1);
    if (w2 && h2) up_add(0, 0, w2 - 1, h2 - 1);
}

/* ---- helpers ----------------------------------------------------------- */
static void vk_log(const char *msg) { fprintf(stdout, "psxrecomp: vulkan: %s\n", msg); }

static int vk_die(const char *why) { fprintf(stdout, "psxrecomp: vulkan: %s\n", why); return 0; }

static uint32_t find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < s_memprops.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (s_memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

static void vk_gpu_sync_internal(void);   /* drain queue + reclaim work pool/staging */

/* Every draw goes through here so vk_frame_perf's draw-call count is exact. */
static void perf_draw(VkCommandBuffer cb, uint32_t vertex_count,
                      uint32_t instance_count, uint32_t first_vertex,
                      uint32_t first_instance) {
    s_perf_cur.draws++;
    p_vkCmdDraw(cb, vertex_count, instance_count, first_vertex, first_instance);
}

/* Coalesce work operations into one command buffer per sync interval. This
 * preserves recording order while removing a queue submission per small GPU
 * operation. */
static VkCommandBuffer s_work_cb;

static VkCommandBuffer begin_oneshot(void) {
    s_perf_cur.oneshots++;
    if (s_work_cb) return s_work_cb;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_work_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    if (p_vkAllocateCommandBuffers(s_dev, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(cb, &bi);
    perf_ts_scene_begin(cb);   /* first CB of the frame: GPU scene-start stamp */
    s_work_cb = cb;
    return cb;
}

/* Finish a logical operation but keep the shared command buffer open. */
static void end_oneshot(VkCommandBuffer cb) {
    (void)cb;
    s_work_pending = 1;
}

static void flush_work(void) {
    if (!s_work_cb) return;
    p_vkEndCommandBuffer(s_work_cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_work_cb;
    p_vkQueueSubmit(s_queue, 1, &si, VK_NULL_HANDLE);
    s_perf_cur.submits++;
    s_work_cb = VK_NULL_HANDLE;
}

/* Defer a staging buffer's destruction until the queue is next idle (its
 * copy may still be in flight). */
static void defer_staging(VkBuffer buf, VkDeviceMemory mem) {
    if (s_pending_n >= PENDING_STAGING_MAX)
        vk_gpu_sync_internal();   /* ring full mid-frame: drain so we never leak */
    s_pending_buf[s_pending_n] = buf; s_pending_mem[s_pending_n] = mem; s_pending_n++;
}

/* Drain all submitted work, then reclaim the work-CB pool and deferred staging.
 * The single sync point for the deferred-submit model. */
static void vk_gpu_sync_internal(void) {
    s_perf_cur.syncs++;
    flush_work();
    if (s_work_pending) {
        uint64_t wait_start = SDL_GetPerformanceCounter();
        p_vkQueueWaitIdle(s_queue);
        s_perf_cur.wait_us += perf_elapsed_us(wait_start);
        /* RELEASE_RESOURCES so the per-op command buffers allocated since the
         * last sync are actually freed (a plain reset only recycles them, and
         * begin_oneshot always allocates fresh -> unbounded growth). */
        p_vkResetCommandPool(s_dev, s_work_pool, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
        s_work_pending = 0;
    }
    for (int i = 0; i < s_pending_n; i++) {
        staging_release(s_pending_buf[i], s_pending_mem[i]);
    }
    s_pending_n = 0;
    s_ds_blit_idx = 0;
    /* Queue idle: every submitted draw has consumed its vertex-buffer region,
     * so recycle the sub-allocation cursors. An OPEN (unflushed) batch's
     * vertices move to offset 0 so its eventual draw stays contiguous. */
    if (s_vmap) {
        if (s_vcount && s_vbase)
            memmove(s_vmap, s_vmap + s_vbase, (size_t)s_vcount * sizeof(Vert));
        s_vbase = 0;
    }
    if (s_tmap) {
        if (s_tcount && s_tbase)
            memmove(s_tmap, s_tmap + s_tbase, (size_t)s_tcount * sizeof(TexVert));
        s_tbase = 0;
    }
}
#define gpu_sync() vk_gpu_sync_internal()

/* Image layout transition (full subresource, color aspect). */
static void img_barrier(VkCommandBuffer cb, VkImage img,
                        VkImageLayout from, VkImageLayout to,
                        VkAccessFlags src_access, VkAccessFlags dst_access,
                        VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
    p_vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Map a layout to its (access mask, pipeline stage) for barriers. */
static void layout_access(VkImageLayout l, VkAccessFlags *acc, VkPipelineStageFlags *stage) {
    switch (l) {
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            *acc = VK_ACCESS_TRANSFER_READ_BIT;  *stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            *acc = VK_ACCESS_TRANSFER_WRITE_BIT; *stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            *acc = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            /* sampled in both the fragment (textured draw) and compute (pack)
             * stages, so cover all commands rather than guess a single stage. */
            *acc = VK_ACCESS_SHADER_READ_BIT; *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; break;
        case VK_IMAGE_LAYOUT_GENERAL:
            *acc = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; break;
        default:
            *acc = 0; *stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; break;
    }
}

/* Transition an arbitrary color-aspect image to `want` on cb, tracking layout
 * through *cur. */
static void img_to(VkCommandBuffer cb, VkImage img, VkImageLayout *cur, VkImageLayout want) {
    if (*cur == want) return;
    VkAccessFlags sa, da; VkPipelineStageFlags ss, ds;
    layout_access(*cur, &sa, &ss);
    layout_access(want, &da, &ds);
    img_barrier(cb, img, *cur, want, sa, da, ss, ds);
    *cur = want;
}

/* Transition the VRAM image to `want` on cb, tracking its current layout. */
static void vram_to(VkCommandBuffer cb, VkImageLayout want) {
    img_to(cb, s_vram_img, &s_vram_layout, want);
}

/* ---- loader bring-up --------------------------------------------------- */
static int load_global_funcs(void) {
#define LOAD(n) p_##n = (PFN_##n)p_vkGetInstanceProcAddr(NULL, #n); if (!p_##n) return vk_die("missing " #n);
    VK_GLOBAL_FUNCS(LOAD)
#undef LOAD
    return 1;
}
static int load_instance_funcs(void) {
#define LOAD(n) p_##n = (PFN_##n)p_vkGetInstanceProcAddr(s_instance, #n); if (!p_##n) return vk_die("missing " #n);
    VK_INSTANCE_FUNCS(LOAD)
#undef LOAD
    p_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        p_vkGetInstanceProcAddr(s_instance, "vkGetDeviceProcAddr");
    return p_vkGetDeviceProcAddr ? 1 : vk_die("missing vkGetDeviceProcAddr");
}
static int load_device_funcs(void) {
#define LOAD(n) p_##n = (PFN_##n)p_vkGetDeviceProcAddr(s_dev, #n); if (!p_##n) return vk_die("missing " #n);
    VK_DEVICE_FUNCS(LOAD)
#undef LOAD
    return 1;
}

/* ---- instance / device / swapchain ------------------------------------- */
static int create_instance(void) {
    unsigned ext_count = 0;
#if defined(PSX_SDL3)
    const char *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!sdl_exts)
        return vk_die("SDL_Vulkan_GetInstanceExtensions failed");
    const char *exts[16];
    if (ext_count > 16) ext_count = 16;
    for (unsigned i = 0; i < ext_count; ++i) exts[i] = sdl_exts[i];
#else
    if (!SDL_Vulkan_GetInstanceExtensions(s_win, &ext_count, NULL))
        return vk_die("SDL_Vulkan_GetInstanceExtensions(count) failed");
    const char *exts[16];
    if (ext_count > 16) ext_count = 16;
    if (!SDL_Vulkan_GetInstanceExtensions(s_win, &ext_count, exts))
        return vk_die("SDL_Vulkan_GetInstanceExtensions(list) failed");
#endif

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "psxrecomp";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = exts;
    if (p_vkCreateInstance(&ci, NULL, &s_instance) != VK_SUCCESS)
        return vk_die("vkCreateInstance failed");
    return 1;
}

static int pick_device(void) {
    uint32_t n = 0;
    p_vkEnumeratePhysicalDevices(s_instance, &n, NULL);
    if (!n) return vk_die("no Vulkan physical devices");
    if (n > 8) n = 8;
    VkPhysicalDevice devs[8];
    p_vkEnumeratePhysicalDevices(s_instance, &n, devs);

    /* Prefer a discrete GPU with a graphics+present queue family. */
    VkPhysicalDevice best = VK_NULL_HANDLE; uint32_t best_fam = 0; int best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pr; p_vkGetPhysicalDeviceProperties(devs[i], &pr);
        uint32_t qn = 0; p_vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        if (qn > 16) qn = 16;
        VkQueueFamilyProperties qf[16];
        p_vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qf);
        for (uint32_t f = 0; f < qn; f++) {
            VkBool32 present = VK_FALSE;
            p_vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], f, s_surface, &present);
            if ((qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                int score = (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2 : 1;
                if (score > best_score) { best_score = score; best = devs[i]; best_fam = f; }
                break;
            }
        }
    }
    if (best == VK_NULL_HANDLE) return vk_die("no graphics+present queue family");
    s_phys = best; s_qfam = best_fam;
    p_vkGetPhysicalDeviceMemoryProperties(s_phys, &s_memprops);
    VkPhysicalDeviceProperties pr; p_vkGetPhysicalDeviceProperties(s_phys, &pr);
    fprintf(stdout, "psxrecomp: vulkan: device = %s\n", pr.deviceName);
    /* Timestamp queries (vk_frame_perf): period in ns/tick, plus the device's
     * image-size ceiling for the windowed target. */
    s_ts_period_ns = (float)pr.limits.timestampPeriod;
    s_ts_ok = pr.limits.timestampComputeAndGraphics &&
              pr.limits.timestampPeriod > 0.0f;
    s_max_image_dim = (int)pr.limits.maxImageDimension2D;
    return 1;
}

static int create_device(void) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    q.queueFamilyIndex = s_qfam; q.queueCount = 1; q.pQueuePriorities = &prio;
    const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = dev_exts;
    if (p_vkCreateDevice(s_phys, &ci, NULL, &s_dev) != VK_SUCCESS)
        return vk_die("vkCreateDevice failed");
    if (!load_device_funcs()) return 0;
    p_vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
    return 1;
}

static VkPresentModeKHR choose_present_mode(void) {
    uint32_t n = 0;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(s_phys, s_surface, &n, NULL);
    if (n > 8) n = 8;
    VkPresentModeKHR modes[8];
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(s_phys, s_surface, &n, modes);
    /* The frontend already owns frame cadence. Prefer MAILBOX for tear-free
     * delivery so FIFO does not introduce a second blocking pacer. */
    VkPresentModeKHR want = (s_present_mode_req == 0)
                          ? VK_PRESENT_MODE_IMMEDIATE_KHR
                          : VK_PRESENT_MODE_MAILBOX_KHR;
    for (uint32_t i = 0; i < n; i++) if (modes[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;  /* always supported */
}

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s_surface, &caps);

    uint32_t fn = 0;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surface, &fn, NULL);
    if (fn > 16) fn = 16;
    VkSurfaceFormatKHR fmts[16];
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surface, &fn, fmts);
    VkSurfaceFormatKHR chosen = fmts[0];
    for (uint32_t i = 0; i < fn; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = fmts[i]; break; }
    }
    s_sc_format = chosen.format;

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
#if defined(PSX_SDL3)
        SDL_GetWindowSizeInPixels(s_win, &w, &h);
#else
        SDL_Vulkan_GetDrawableSize(s_win, &w, &h);
#endif
        ext.width = (uint32_t)w; ext.height = (uint32_t)h;
    }
    s_sc_extent = ext;

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount && img_count > caps.maxImageCount) img_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = s_surface;
    ci.minImageCount = img_count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = choose_present_mode();
    ci.clipped = VK_TRUE;
    if (p_vkCreateSwapchainKHR(s_dev, &ci, NULL, &s_swapchain) != VK_SUCCESS)
        return vk_die("vkCreateSwapchainKHR failed");

    s_sc_count = 0;
    p_vkGetSwapchainImagesKHR(s_dev, s_swapchain, &s_sc_count, NULL);
    if (s_sc_count > 8) s_sc_count = 8;
    p_vkGetSwapchainImagesKHR(s_dev, s_swapchain, &s_sc_count, s_sc_images);
    return 1;
}

static void pp_release_swapchain(void);
static void pp_destroy_all(void);
static void ri_release_swapchain(void);
static void ri_destroy_all(void);
static void vfx_release_swapchain(void);   /* [video] volumetrics, same lifetime */

static void destroy_swapchain(void) {
    pp_release_swapchain();   /* present-pass views/framebuffers/pipeline */
    if (s_swapchain) { p_vkDestroySwapchainKHR(s_dev, s_swapchain, NULL); s_swapchain = VK_NULL_HANDLE; }
}

static int create_sync(void) {
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = VK_FRAMES;
    if (p_vkAllocateCommandBuffers(s_dev, &ai, s_cmd) != VK_SUCCESS)
        return vk_die("vkAllocateCommandBuffers failed");
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < VK_FRAMES; i++) {
        if (p_vkCreateSemaphore(s_dev, &si, NULL, &s_sem_acquire[i]) != VK_SUCCESS ||
            p_vkCreateSemaphore(s_dev, &si, NULL, &s_sem_render[i]) != VK_SUCCESS ||
            p_vkCreateFence(s_dev, &fi, NULL, &s_fence[i]) != VK_SUCCESS)
            return vk_die("sync object creation failed");
    }
    return 1;
}

/* ---- windowed render target ------------------------------------------------
 * The authoritative VRAM is the native R16 raw mirror (textures, readback,
 * guest VRAM reads). The RGBA8 target only has to hold what can be DISPLAYED
 * at scale S, so it is allocated over the native rect the game actually
 * displays/draws into instead of all 1024x512 — the GL backend's
 * high-resolution window (s_hiw) with a single rect instead of tiles.
 * WE2002 keeps two 512x240 framebuffers at (0,0) and (0,240): 47% of VRAM.
 *
 * Coordinates stay native everywhere: viewports and scissors are translated by
 * -origin (negative viewport origin is legal in Vulkan 1.1), so the shaders,
 * the wide mirror and the pack pass keep working in VRAM space.
 *
 * Writes outside the target are clipped and counted (vk_frame_perf.oob).
 * Draws and the presented rect GROW the target (union, up to the whole VRAM);
 * CPU uploads and VRAM copies only touch the part inside it — the raw mirror
 * always receives those exactly, so texturing and readback stay complete. */
/* The full-VRAM render target is the shipped mode. The windowed target
 * (s_tgt_full_env == 0) is kept for measurement, but it renders only the
 * framebuffer fill on the AMD driver after a resize: the frame's draws never
 * reach the image the present reads, while the SAME frame is correct with the
 * full target (target dumps + present_shot in docs/VULKAN_PARITY.md). Do not
 * make it the default again until that is root-caused. */
static int s_tgt_ready;      /* target images exist */
static int s_tgt_full_env;   /* whole-VRAM target (default) */

static int make_image(VkFormat fmt, int w, int h, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect,
                      VkImage *img, VkDeviceMemory *mem, VkImageView *view);
static void rgb555_to_rgba8(uint16_t c, uint8_t out[4]);

static int tgt_contains(int x0, int y0, int x1, int y1) {
    return s_tgt_ready && x0 >= s_tx0 && y0 >= s_ty0 &&
           x1 <= s_tx0 + s_tw - 1 && y1 <= s_ty0 + s_th - 1;
}

/* Perf: count a clipped write (native rect outside the target). */
static void tgt_note_oob(int x0, int y0, int x1, int y1) {
    if (!tgt_contains(x0, y0, x1, y1)) s_perf_cur.oob++;
}

/* Clip a native rect to the target. Returns 0 when nothing is left. */
static int tgt_clip(int *x0, int *y0, int *x1, int *y1) {
    if (!s_tgt_ready) return 0;
    if (*x0 < s_tx0) *x0 = s_tx0;
    if (*y0 < s_ty0) *y0 = s_ty0;
    if (*x1 > s_tx0 + s_tw - 1) *x1 = s_tx0 + s_tw - 1;
    if (*y1 > s_ty0 + s_th - 1) *y1 = s_ty0 + s_th - 1;
    return *x1 >= *x0 && *y1 >= *y0;
}

/* Re-point the descriptor sets that name the target image (pack reads hr). */
static void tgt_set_bindings(void);

static void tgt_destroy(void) {
    if (s_fbo)         { p_vkDestroyFramebuffer(s_dev, s_fbo, NULL); s_fbo = VK_NULL_HANDLE; }
    if (s_vram_view)   { p_vkDestroyImageView(s_dev, s_vram_view, NULL); s_vram_view = VK_NULL_HANDLE; }
    if (s_vram_img)    { p_vkDestroyImage(s_dev, s_vram_img, NULL); s_vram_img = VK_NULL_HANDLE; }
    if (s_vram_mem)    { p_vkFreeMemory(s_dev, s_vram_mem, NULL); s_vram_mem = VK_NULL_HANDLE; }
    if (s_ds_view)     { p_vkDestroyImageView(s_dev, s_ds_view, NULL); s_ds_view = VK_NULL_HANDLE; }
    if (s_ds_sample_view) { p_vkDestroyImageView(s_dev, s_ds_sample_view, NULL); s_ds_sample_view = VK_NULL_HANDLE; }
    if (s_ds_img)      { p_vkDestroyImage(s_dev, s_ds_img, NULL); s_ds_img = VK_NULL_HANDLE; }
    if (s_ds_mem)      { p_vkFreeMemory(s_dev, s_ds_mem, NULL); s_ds_mem = VK_NULL_HANDLE; }
    if (s_scratch_view){ p_vkDestroyImageView(s_dev, s_scratch_view, NULL); s_scratch_view = VK_NULL_HANDLE; }
    if (s_scratch_img) { p_vkDestroyImage(s_dev, s_scratch_img, NULL); s_scratch_img = VK_NULL_HANDLE; }
    if (s_scratch_mem) { p_vkFreeMemory(s_dev, s_scratch_mem, NULL); s_scratch_mem = VK_NULL_HANDLE; }
    s_vram_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_scratch_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_tgt_ready = 0;
}

/* Create the target over native [x0,y0]-[x1,y1] (inclusive). Grows keep the
 * overlapping pixels: the old colour image is blitted and the old stencil
 * copied into the new one, so a grown target is never visually reset. */
static int tgt_create(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    if (x1 < x0 || y1 < y0) return 0;
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    /* Hard device limit: step the scale down instead of failing the target. */
    while (s_scale > 1 && (w * s_scale > s_max_image_dim || h * s_scale > s_max_image_dim)) {
        s_scale--;
    }
    int S = s_scale;

    VkImage old_img = s_vram_img, old_ds = s_ds_img;
    VkDeviceMemory old_mem = s_vram_mem, old_dsmem = s_ds_mem;
    VkImageView old_view = s_vram_view, old_dsview = s_ds_view;
    VkFramebuffer old_fbo = s_fbo;
    VkImage old_scratch = s_scratch_img;
    VkDeviceMemory old_scratchmem = s_scratch_mem;
    VkImageView old_scratchview = s_scratch_view;
    int ox0 = s_tx0, oy0 = s_ty0, ow = s_tw, oh = s_th;
    int had_old = s_tgt_ready;

    s_vram_img = VK_NULL_HANDLE; s_vram_mem = VK_NULL_HANDLE; s_vram_view = VK_NULL_HANDLE;
    s_ds_img = VK_NULL_HANDLE; s_ds_mem = VK_NULL_HANDLE; s_ds_view = VK_NULL_HANDLE;
    s_scratch_img = VK_NULL_HANDLE; s_scratch_mem = VK_NULL_HANDLE; s_scratch_view = VK_NULL_HANDLE;
    s_fbo = VK_NULL_HANDLE;
    s_vram_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_scratch_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    /* Colour target: render attachment + present source + copy/blit source. */
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent.width = (uint32_t)(w * S);
    ci.extent.height = (uint32_t)(h * S);
    ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(s_dev, &ci, NULL, &s_vram_img) != VK_SUCCESS)
        return vk_die("vram image create failed");
    VkMemoryRequirements req; p_vkGetImageMemoryRequirements(s_dev, s_vram_img, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mi.memoryTypeIndex == UINT32_MAX ||
        p_vkAllocateMemory(s_dev, &mi, NULL, &s_vram_mem) != VK_SUCCESS)
        return vk_die("vram memory alloc failed");
    p_vkBindImageMemory(s_dev, s_vram_img, s_vram_mem, 0);
    {
        VkImageViewCreateInfo vv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vv.image = s_vram_img; vv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vv.format = VK_FORMAT_R8G8B8A8_UNORM;
        vv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vv.subresourceRange.levelCount = 1; vv.subresourceRange.layerCount = 1;
        if (p_vkCreateImageView(s_dev, &vv, NULL, &s_vram_view) != VK_SUCCESS)
            return vk_die("vram image view failed");
    }
    if (!make_image(s_ds_format, w * S, h * S,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    /* [video] volumetrics samples the depth aspect as the fog
                     * depth proxy in the present pass (docs/EFFECTS.md). */
                    VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    DS_ASPECT, &s_ds_img, &s_ds_mem, &s_ds_view)) return 0;
    if (s_vfx_fog) {
        /* Depth aspect only: a combined depth/stencil view is not sampleable. */
        VkImageViewCreateInfo dv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        dv.image = s_ds_img; dv.viewType = VK_IMAGE_VIEW_TYPE_2D; dv.format = s_ds_format;
        dv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        dv.subresourceRange.levelCount = 1; dv.subresourceRange.layerCount = 1;
        if (p_vkCreateImageView(s_dev, &dv, NULL, &s_ds_sample_view) != VK_SUCCESS)
            return vk_die("ds sample view failed");
    }
    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, w * S, h * S,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_scratch_img, &s_scratch_mem, &s_scratch_view)) return 0;
    {
        VkImageView views[2] = { s_vram_view, s_ds_view };
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = s_rpass; fi.attachmentCount = 2; fi.pAttachments = views;
        fi.width = (uint32_t)(w * S); fi.height = (uint32_t)(h * S); fi.layers = 1;
        if (p_vkCreateFramebuffer(s_dev, &fi, NULL, &s_fbo) != VK_SUCCESS)
            return vk_die("framebuffer failed");
    }
    s_tx0 = x0; s_ty0 = y0; s_tw = w; s_th = h;
    s_tgt_pct = (float)((double)w * h / ((double)VRAM_W * VRAM_H));
    s_tgt_ready = 1;

    /* Clear the new colour + stencil, copy the overlap from the old target,
     * then leave both in the layouts the rest of the file expects. */
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_vram_img, &s_vram_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0,0,0,0}};
    VkImageSubresourceRange crng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(cb, s_vram_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &crng);
    {
        VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        db.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = s_ds_img; db.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        db.subresourceRange.aspectMask = DS_ASPECT;
        db.subresourceRange.levelCount = 1; db.subresourceRange.layerCount = 1;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &db);
        VkClearDepthStencilValue dsv = { 0.0f, 0 };
        VkImageSubresourceRange srng = { DS_ASPECT, 0, 1, 0, 1 };
        p_vkCmdClearDepthStencilImage(cb, s_ds_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      &dsv, 1, &srng);
    }
    if (had_old && old_img) {
        /* Copy the overlap only: a SHRINK makes the old image larger than the
         * new one, and a blit outside the destination is invalid. */
        int cx0 = ox0 > x0 ? ox0 : x0, cy0 = oy0 > y0 ? oy0 : y0;
        int cx1 = (ox0 + ow - 1) < x1 ? (ox0 + ow - 1) : x1;
        int cy1 = (oy0 + oh - 1) < y1 ? (oy0 + oh - 1) : y1;
        if (cx1 < cx0 || cy1 < cy0) { cx0 = 0; cx1 = -1; cy0 = 0; cy1 = -1; }
        /* Colour: same scale, so a blit is an exact pixel copy. Order it after
         * the clear (transfer write -> transfer write on the same image). */
        {
            VkImageMemoryBarrier wb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            wb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            wb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            wb.srcQueueFamilyIndex = wb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            wb.image = s_vram_img;
            wb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            wb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            wb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            wb.subresourceRange.levelCount = 1; wb.subresourceRange.layerCount = 1;
            p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &wb);
        }
        VkImageMemoryBarrier ob = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        ob.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ob.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ob.srcQueueFamilyIndex = ob.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ob.image = old_img;
        ob.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_SHADER_WRITE_BIT;
        ob.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ob.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ob.subresourceRange.levelCount = 1; ob.subresourceRange.layerCount = 1;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ob);
        VkImageBlit blit = {0};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0].x = (cx0 - ox0) * S; blit.srcOffsets[0].y = (cy0 - oy0) * S;
        blit.srcOffsets[1].x = (cx1 + 1 - ox0) * S;
        blit.srcOffsets[1].y = (cy1 + 1 - oy0) * S;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[0].x = (cx0 - x0) * S; blit.dstOffsets[0].y = (cy0 - y0) * S;
        blit.dstOffsets[1].x = (cx1 + 1 - x0) * S;
        blit.dstOffsets[1].y = (cy1 + 1 - y0) * S;
        blit.dstOffsets[1].z = 1;
        p_vkCmdBlitImage(cb, old_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         s_vram_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &blit, VK_FILTER_NEAREST);
        /* Stencil (mask bits) + depth: straight image copy, same format. */
        /* Stencil (mask bits): the NEW image was already cleared to 0 above.
         * Do NOT copy the old stencil: it is per-frame framebuffer state, and
         * an overwrite/grow that happens while a frame is in flight copied a
         * partially written mask into the new target. Once a single mask bit
         * came across wrong every CHECK draw fails (the stencil test is always
         * enabled), which is exactly the "only the fill renders" state seen
         * after a windowed-target resize. The fresh target starts with a clear
         * mask, like the GL FBO that is reallocated at the same point. */
    }
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    {
        VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        db.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        db.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = s_ds_img;
        db.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        db.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        db.subresourceRange.aspectMask = DS_ASPECT;
        db.subresourceRange.levelCount = 1; db.subresourceRange.layerCount = 1;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &db);
    }
    s_vfx_ds_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    end_oneshot(cb);
    gpu_sync();   /* the copy into the new images must complete before the old go */

    if (old_img)  p_vkDestroyImage(s_dev, old_img, NULL);
    if (old_mem)  p_vkFreeMemory(s_dev, old_mem, NULL);
    if (old_view) p_vkDestroyImageView(s_dev, old_view, NULL);
    if (old_ds)   p_vkDestroyImage(s_dev, old_ds, NULL);
    if (old_dsmem)p_vkFreeMemory(s_dev, old_dsmem, NULL);
    if (old_dsview)p_vkDestroyImageView(s_dev, old_dsview, NULL);
    if (old_scratch) p_vkDestroyImage(s_dev, old_scratch, NULL);
    if (old_scratchmem) p_vkFreeMemory(s_dev, old_scratchmem, NULL);
    if (old_scratchview) p_vkDestroyImageView(s_dev, old_scratchview, NULL);
    if (old_fbo)  p_vkDestroyFramebuffer(s_dev, old_fbo, NULL);
    tgt_set_bindings();
    return 1;
}

static void begin_geo_pass(VkCommandBuffer cb);
static void set_vp_full(VkCommandBuffer cb);
static void set_scissor_px(VkCommandBuffer cb, int x, int y, int w, int h);
static float px_shift(void);
static void bind_masked(VkCommandBuffer cb, int prog, int topo, int blend,
                        int check, int write_val);

/* ---- push-constant blocks (must match the shaders' layouts) -------------
 * Declared before the target machinery: the first-creation seed blit needs
 * BlitPush. */
typedef struct { float shift, xoff, xhalf, fog_z; } GeoPush;   /* 16 bytes */
typedef struct {
    float shift, xoff, xhalf;        /* 0,4,8 */
    int   semipass, maskset, filter; /* 12,16,20 */
    float fog_z;                     /* 24: [video] volumetrics depth proxy,
                                      *     < 0 = off (clip z stays 0.0) */
    int   _pad;                      /* 28 -> ivec4 16-byte align */
    int   twin[4];                   /* 32..47 */
} TexPush;                           /* 48 bytes */
typedef struct {
    float shift;        /* 0 */
    int   stp_pass;     /* 4 */
    int   maskset;      /* 8 */
    int   src_div;      /* 12 */
    int   src_off[2];   /* 16,20 */
    int   _pad[2];      /* 24,28 -> ivec4 16-byte align */
    int   rect[4];      /* 32..47: x0,y0,x1,y1 native px */
} BlitPush;             /* 48 bytes */
typedef struct { int scale, off_x, off_y, tgt_x, tgt_y,
                 ext_x0, ext_x1, ext_y0, ext_y1, ext_off; } PackPush;   /* 40 bytes */

/* First-creation seed: the CPU VRAM array holds every CPU-side pixel write the
 * game made (fills, uploads, MDEC blocks) — including writes made before the
 * target existed, which only landed in the raw mirror. Draw those at S so the
 * first presented frame is not missing its filled background. Runs on its own
 * command buffer; the caller's batch queues are untouched. */
static void tgt_seed_from_cpu(int x0, int y0, int w, int h) {
    if (!s_vram || !s_tgt_ready || w <= 0 || h <= 0) return;
    VkBuffer buf; VkDeviceMemory mem; void *map;
    if (!make_staging((VkDeviceSize)w * h * 4, &buf, &mem, &map)) return;
    uint8_t *m = (uint8_t *)map;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            rgb555_to_rgba8(s_vram[(size_t)(y0 + r) * VRAM_W + x0 + c],
                            m + ((size_t)r * w + c) * 4);
    if (s_ds_blit_idx >= BLIT_DESC_RING) gpu_sync();
    VkDescriptorSet ds = s_ds_blit_ring[s_ds_blit_idx++];
    VkDescriptorImageInfo ii = { s_samp, s_up_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ds; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    p_vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);

    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_up_img, &s_up_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy uc = {0};
    uc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    uc.imageSubresource.layerCount = 1;
    uc.imageOffset.x = x0; uc.imageOffset.y = y0;
    uc.imageExtent.width = (uint32_t)w; uc.imageExtent.height = (uint32_t)h;
    uc.imageExtent.depth = 1;
    p_vkCmdCopyBufferToImage(cb, buf, s_up_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uc);
    img_to(cb, s_up_img, &s_up_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vram_to(cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_geo_pass(cb);
    set_vp_full(cb); set_scissor_px(cb, x0, y0, w, h);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl_blit, 0, 1, &ds, 0, NULL);
    BlitPush bp = {0};
    bp.shift = px_shift(); bp.maskset = 0; bp.src_div = s_scale;
    bp.src_off[0] = s_tx0; bp.src_off[1] = s_ty0;   /* target-local fragcoord */
    bp.rect[0] = x0; bp.rect[1] = y0; bp.rect[2] = x0 + w; bp.rect[3] = y0 + h;
    bind_masked(cb, 2, 0, 0, 0, 0);
    bp.stp_pass = 1;
    p_vkCmdPushConstants(cb, s_pl_blit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
    perf_draw(cb, 6, 1, 0, 0);
    bind_masked(cb, 2, 0, 0, 0, 1);
    bp.stp_pass = 2;
    p_vkCmdPushConstants(cb, s_pl_blit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
    perf_draw(cb, 6, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
    defer_staging(buf, mem);
    s_gpu_dirty = 1;
}

/* Native union of the RECENT display rects (last DU_N presents). Draws are
 * only required to land inside this: pixels outside it can never be displayed,
 * while the raw mirror still receives them exactly (textures, readback). A
 * rolling window (not a running union) matters because a transient 1024-wide
 * boot mode would otherwise pin the target at full VRAM for the session. */
#define DU_N 8
static int s_du_r[DU_N][4];
static int s_du_n, s_du_head;
static int s_du_x0, s_du_y0, s_du_x1 = -1, s_du_y1 = -1;   /* empty */

static int du_empty(void) { return s_du_x1 < s_du_x0; }

static void du_add(int x0, int y0, int x1, int y1) {
    int *r = s_du_r[s_du_head];
    r[0] = x0; r[1] = y0; r[2] = x1; r[3] = y1;
    s_du_head = (s_du_head + 1) % DU_N;
    if (s_du_n < DU_N) s_du_n++;
    s_du_x0 = s_du_r[0][0]; s_du_y0 = s_du_r[0][1];
    s_du_x1 = s_du_r[0][2]; s_du_y1 = s_du_r[0][3];
    for (int i = 1; i < s_du_n; i++) {
        const int *q = s_du_r[i];
        if (q[0] < s_du_x0) s_du_x0 = q[0];
        if (q[1] < s_du_y0) s_du_y0 = q[1];
        if (q[2] > s_du_x1) s_du_x1 = q[2];
        if (q[3] > s_du_y1) s_du_y1 = q[3];
    }
}

/* Intersect `r` with the display union (may report empty). */
static int du_intersect(int *x0, int *y0, int *x1, int *y1) {
    if (du_empty()) return 0;
    if (*x0 < s_du_x0) *x0 = s_du_x0;
    if (*y0 < s_du_y0) *y0 = s_du_y0;
    if (*x1 > s_du_x1) *x1 = s_du_x1;
    if (*y1 > s_du_y1) *y1 = s_du_y1;
    return *x1 >= *x0 && *y1 >= *y0;
}

/* Grow (or create) the target so it holds native rect [x0,y0]-[x1,y1]. Called
 * before a batch/display is recorded; never call it mid-record. */
static int tgt_require(int x0, int y0, int x1, int y1) {
    if (tgt_contains(x0, y0, x1, y1)) return 1;
    if (!s_ctx_ok || !s_ready) return 0;
    if (s_tgt_full_env) {   /* whole-VRAM target (the default) */
        if (!tgt_contains(0, 0, VRAM_W - 1, VRAM_H - 1)) {
            int first = !s_tgt_ready;
            gpu_sync();
            if (!tgt_create(0, 0, VRAM_W - 1, VRAM_H - 1)) return 0;
            if (first) tgt_seed_from_cpu(0, 0, VRAM_W, VRAM_H);
            return 1;
        }
        return 0;
    }
    int nx0 = s_tgt_ready ? s_tx0 : x0, ny0 = s_tgt_ready ? s_ty0 : y0;
    int nx1 = s_tgt_ready ? s_tx0 + s_tw - 1 : x1;
    int ny1 = s_tgt_ready ? s_ty0 + s_th - 1 : y1;
    if (x0 < nx0) nx0 = x0;
    if (y0 < ny0) ny0 = y0;
    if (x1 > nx1) nx1 = x1;
    if (y1 > ny1) ny1 = y1;
    if (nx0 < 0) nx0 = 0;
    if (ny0 < 0) ny0 = 0;
    if (nx1 > VRAM_W - 1) nx1 = VRAM_W - 1;
    if (ny1 > VRAM_H - 1) ny1 = VRAM_H - 1;
    /* Round out to 32 px so a handful of small first draws do not each pay a
     * reallocation; then give up on windowing once the union stops paying. */
    nx0 &= ~31; ny0 &= ~31;
    nx1 = (nx1 + 32) & ~31; if (nx1 > VRAM_W - 1) nx1 = VRAM_W - 1;
    ny1 = (ny1 + 32) & ~31; if (ny1 > VRAM_H - 1) ny1 = VRAM_H - 1;
    double frac = (double)(nx1 - nx0 + 1) * (ny1 - ny0 + 1) /
                  ((double)VRAM_W * VRAM_H);
    if (frac >= 0.9 || s_tgt_grows >= 32) {
        nx0 = 0; ny0 = 0; nx1 = VRAM_W - 1; ny1 = VRAM_H - 1;
        if (s_tgt_grows >= 32)
            fprintf(stdout, "psxrecomp: vulkan: target growth budget spent; "
                            "using the full-VRAM target\n");
    }
    int first = !s_tgt_ready;
    s_tgt_grows++;
    gpu_sync();   /* drain in-flight users before the images are replaced */
    if (!tgt_create(nx0, ny0, nx1, ny1)) return 0;
    if (first) tgt_seed_from_cpu(nx0, ny0, nx1 - nx0 + 1, ny1 - ny0 + 1);
    s_tgt_last_change = s_present_idx;
    fprintf(stdout, "psxrecomp: vulkan: target %dx%d native (%d,%d) at %dx "
                    "= %.0f%% of full VRAM\n",
            s_tw, s_th, s_tx0, s_ty0, s_scale,
            100.0 * (double)s_tw * s_th / ((double)VRAM_W * VRAM_H));
    fflush(stdout);
    return 1;
}

/* Present path: extend the recent-display union with this frame's rect, then
 * make sure the target covers it. A target that is much larger than the recent
 * union (a transient boot mode grew it) shrinks back, keeping the pixels the
 * two rects share. */
static int tgt_require_display(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    if (x1 < x0 || y1 < y0) return 0;
    du_add(x0, y0, x1, y1);
    int ux0 = s_du_x0, uy0 = s_du_y0, ux1 = s_du_x1, uy1 = s_du_y1;
    if (tgt_contains(ux0, uy0, ux1, uy1) && s_tgt_ready && !s_tgt_full_env) {
        double tarea = (double)s_tw * s_th;
        double uarea = (double)(ux1 - ux0 + 1) * (uy1 - uy0 + 1);
        if (tarea > 1.6 * uarea && s_tgt_shrinks < 4 &&
            s_present_idx > s_tgt_last_change + 180) {
            int nx0 = ux0 & ~31, ny0 = uy0 & ~31;
            int nx1 = (ux1 + 32) & ~31, ny1 = (uy1 + 32) & ~31;
            if (nx1 > VRAM_W - 1) nx1 = VRAM_W - 1;
            if (ny1 > VRAM_H - 1) ny1 = VRAM_H - 1;
            s_tgt_shrinks++;
            s_tgt_grows++;
            gpu_sync();
            if (tgt_create(nx0, ny0, nx1, ny1)) {
                s_tgt_last_change = s_present_idx;
                fprintf(stdout, "psxrecomp: vulkan: target shrunk to %dx%d native "
                                "(%d,%d) = %.0f%% of full VRAM\n",
                        s_tw, s_th, s_tx0, s_ty0, 100.0 * (double)s_tw * s_th /
                        ((double)VRAM_W * VRAM_H));
                fflush(stdout);
            }
            return 1;
        }
    }
    return tgt_require(ux0, uy0, ux1, uy1);
}

/* Write path: a batch only needs target space where it can become visible
 * (the displayed union). Outside it the write is clipped and the raw mirror
 * keeps the native value, exactly like a texture page. */
static int tgt_require_write(int x0, int y0, int x1, int y1) {
    if (du_empty()) return s_tgt_ready ? 1 : 0;
    if (!du_intersect(&x0, &y0, &x1, &y1)) return s_tgt_ready ? 1 : 0;
    return tgt_require(x0, y0, x1, y1);
}


/* ---- geometry pipeline (Phase 2) --------------------------------------- */
static VkShaderModule make_module(const uint32_t *code, uint32_t size) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size; ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    p_vkCreateShaderModule(s_dev, &ci, NULL, &m);
    return m;
}

/* ---- push-constant blocks live above (see the target machinery) --------- */

/* Create a device-local image + view (color aspect by default). */
static int make_image(VkFormat fmt, int w, int h, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect,
                      VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(s_dev, &ci, NULL, img) != VK_SUCCESS) return vk_die("image create failed");
    VkMemoryRequirements req; p_vkGetImageMemoryRequirements(s_dev, *img, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = find_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mi.memoryTypeIndex == UINT32_MAX || p_vkAllocateMemory(s_dev, &mi, NULL, mem) != VK_SUCCESS)
        return vk_die("image memory alloc failed");
    p_vkBindImageMemory(s_dev, *img, *mem, 0);
    if (view) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange.aspectMask = aspect;
        vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        if (p_vkCreateImageView(s_dev, &vi, NULL, view) != VK_SUCCESS)
            return vk_die("image view failed");
    }
    return 1;
}

static VkFormat choose_ds_format(void) {
    VkFormat cand[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    for (int i = 0; i < 2; i++) {
        VkFormatProperties fp; p_vkGetPhysicalDeviceFormatProperties(s_phys, cand[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return cand[i];
    }
    return VK_FORMAT_D24_UNORM_S8_UINT;
}

static int create_buffer_mapped(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (p_vkCreateBuffer(s_dev, &bci, NULL, buf) != VK_SUCCESS) return vk_die("vertex buffer failed");
    VkMemoryRequirements req; p_vkGetBufferMemoryRequirements(s_dev, *buf, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = find_mem_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mi.memoryTypeIndex == UINT32_MAX || p_vkAllocateMemory(s_dev, &mi, NULL, mem) != VK_SUCCESS)
        return vk_die("vertex buffer memory failed");
    p_vkBindBufferMemory(s_dev, *buf, *mem, 0);
    p_vkMapMemory(s_dev, *mem, 0, size, 0, map);
    return 1;
}

/* Build all render targets, samplers, descriptors, pipeline layouts and the
 * pack compute pipeline. Graphics pipelines are built lazily by get_pipeline. */
static int create_render_targets(void) {
    /* Native raw mirror (sampled + storage) and the native-res upload image.
     * Everything sized by the *display* scale lives in the windowed target and
     * is created on the first draw/present (tgt_create). */
    if (!make_image(VK_FORMAT_R16_UINT, VRAM_HOST_W, VRAM_H,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_raw_img, &s_raw_mem, &s_raw_view)) return 0;
    s_ds_format = choose_ds_format();
    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, VRAM_W, VRAM_H,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_up_img, &s_up_mem, &s_up_view)) return 0;

    /* Clear raw mirror to 0 (park sampled). */
    {
        VkCommandBuffer cb = begin_oneshot();
        img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue zero = {{0,0,0,0}};
        VkImageSubresourceRange crng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        p_vkCmdClearColorImage(cb, s_raw_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &crng);
        img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        end_oneshot(cb);
    }

    /* Render pass: color (hr, load/store) + depth-stencil (stencil load/store). */
    VkAttachmentDescription att[2] = {0};
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1].format = s_ds_format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 2; rpi.pAttachments = att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    if (p_vkCreateRenderPass(s_dev, &rpi, NULL, &s_rpass) != VK_SUCCESS)
        return vk_die("render pass failed");

    /* Fog variant ([video] volumetrics): identical attachments, depth CLEARed
     * each pass so the per-primitive depth proxy never carries stale pixels.
     * Only created when the effect is armed, so the off path allocates and
     * records exactly what it did before. */
    if (s_vfx_fog) {
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        if (p_vkCreateRenderPass(s_dev, &rpi, NULL, &s_rpass_fog) != VK_SUCCESS) {
            s_rpass_fog = VK_NULL_HANDLE;
            s_vfx_fog = 0;
            fprintf(stdout, "psxrecomp: vfx: fog disabled (render pass failed)\n");
        }
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (s_vfx_fog && p_vkCreateRenderPass(s_dev, &rpi, NULL, &s_rpass_fog_load) != VK_SUCCESS) {
            s_rpass_fog_load = VK_NULL_HANDLE;
            s_vfx_fog = 0;
            fprintf(stdout, "psxrecomp: vfx: fog disabled (render pass failed)\n");
        }
    }

    /* No framebuffer here: the windowed target (tgt_create) owns the attachment
     * views and creates one per target size. */

    /* Nearest sampler (texelFetch ignores filtering/addressing, but a combined
     * image sampler still needs one). */
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (p_vkCreateSampler(s_dev, &sci, NULL, &s_samp) != VK_SUCCESS) return vk_die("sampler failed");

    /* Descriptor set layouts. */
    {
        VkDescriptorSetLayoutBinding tex_b[2] = {
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
            { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                           VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 2; ci.pBindings = tex_b;
        if (p_vkCreateDescriptorSetLayout(s_dev, &ci, NULL, &s_dsl_tex) != VK_SUCCESS) return vk_die("dsl tex");
        ci.bindingCount = 1; ci.pBindings = &b;
        if (p_vkCreateDescriptorSetLayout(s_dev, &ci, NULL, &s_dsl_blit) != VK_SUCCESS) return vk_die("dsl blit");
        VkDescriptorSetLayoutBinding pb[2] = {
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
        ci.bindingCount = 2; ci.pBindings = pb;
        if (p_vkCreateDescriptorSetLayout(s_dev, &ci, NULL, &s_dsl_pack) != VK_SUCCESS) return vk_die("dsl pack");
    }

    /* 1x1 dummy override image: the fast path binds it at binding 1 and the
     * fragment shader never samples it while v_hd_mode == 0. */
    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_hd_dummy_img, &s_hd_dummy_mem,
                    &s_hd_dummy_view)) return 0;
    {
        VkImageLayout dummy_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkCommandBuffer cb = begin_oneshot();
        img_to(cb, s_hd_dummy_img, &dummy_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue zero = {{0,0,0,0}};
        VkImageSubresourceRange crng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        p_vkCmdClearColorImage(cb, s_hd_dummy_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &crng);
        img_to(cb, s_hd_dummy_img, &dummy_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        end_oneshot(cb);
    }

    /* Descriptor pool + sets: ds_tex (1) + ds_pack (1) + blit ring (BLIT_DESC_RING). */
    {
        /* Budget = exactly what is allocated below: ds_tex has TWO combined
         * image samplers (raw mirror + HD override), ds_pack has one storage
         * image, each of the BLIT_DESC_RING sets has one combined sampler.
         * Allocating a pool to its exact size made p_vkAllocateDescriptorSets
         * fail on the AMD RX 9070 driver (VK_ERROR_OUT_OF_POOL_MEMORY on the
         * last blit sets) and the backend fell back to software with only
         * `vulkan: blit set alloc` in the log. Keep headroom AND report the
         * VkResult: a silent fallback here looks exactly like a working Vulkan
         * build on every other line, so a failure must name its code. */
        const uint32_t kPoolHeadroom = 8;
        VkDescriptorPoolSize sizes[2] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, BLIT_DESC_RING + 2 + kPoolHeadroom },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1 + kPoolHeadroom } };
        VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = BLIT_DESC_RING + 2 + kPoolHeadroom;
        pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (p_vkCreateDescriptorPool(s_dev, &pci, NULL, &s_dpool) != VK_SUCCESS)
            return vk_die("desc pool");
        VkDescriptorSetLayout layouts[2] = { s_dsl_tex, s_dsl_pack };
        VkDescriptorSet *sets[2] = { &s_ds_tex, &s_ds_pack };
        for (int i = 0; i < 2; i++) {
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = s_dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &layouts[i];
            VkResult r = p_vkAllocateDescriptorSets(s_dev, &ai, sets[i]);
            if (r != VK_SUCCESS) {
                fprintf(stdout, "psxrecomp: vulkan: descriptor set %d alloc -> VkResult %d\n", i, (int)r);
                return vk_die("desc set alloc");
            }
        }
        for (int i = 0; i < BLIT_DESC_RING; i++) {
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = s_dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &s_dsl_blit;
            VkResult r = p_vkAllocateDescriptorSets(s_dev, &ai, &s_ds_blit_ring[i]);
            if (r != VK_SUCCESS) {
                fprintf(stdout, "psxrecomp: vulkan: blit set %d/%d alloc -> VkResult %d\n",
                        i, BLIT_DESC_RING, (int)r);
                return vk_die("blit set alloc");
            }
        }
        /* ds_tex: raw mirror sampled + the dummy override image. ds_pack: hr
         * sampled + raw storage; the hr side is (re)written by
         * tgt_set_bindings whenever the target is (re)allocated. */
        VkDescriptorImageInfo raw_smp = { s_samp, s_raw_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo raw_st  = { VK_NULL_HANDLE, s_raw_view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w[2] = {0};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s_ds_tex; w[0].dstBinding = 0;
        w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &raw_smp;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s_ds_pack; w[1].dstBinding = 1;
        w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &raw_st;
        p_vkUpdateDescriptorSets(s_dev, 2, w, 0, NULL);
        hd_write_set(s_ds_tex, s_hd_dummy_view);
    }

    /* HD override descriptor sets: one per cache slot, allocated up front so no
     * set is ever freed while a submitted draw still references it. */
    {
        VkDescriptorPoolSize hd_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         (uint32_t)(2 * VK_HD_CACHE_MAX) };
        VkDescriptorPoolCreateInfo hpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        hpci.maxSets = VK_HD_CACHE_MAX; hpci.poolSizeCount = 1; hpci.pPoolSizes = &hd_size;
        if (p_vkCreateDescriptorPool(s_dev, &hpci, NULL, &s_hd_dpool) != VK_SUCCESS)
            return vk_die("hd desc pool");
        for (int i = 0; i < VK_HD_CACHE_MAX; i++) {
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = s_hd_dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &s_dsl_tex;
            if (p_vkAllocateDescriptorSets(s_dev, &ai, &s_hd_cache[i].set) != VK_SUCCESS)
                return vk_die("hd desc set alloc");
            hd_write_set(s_hd_cache[i].set, s_hd_dummy_view);
        }
    }

    /* Pipeline layouts. */
    {
        VkPushConstantRange pr;
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pr.offset = 0; pr.size = sizeof(GeoPush);
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_pl_geo) != VK_SUCCESS) return vk_die("pl geo");
        pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = sizeof(TexPush);
        li.setLayoutCount = 1; li.pSetLayouts = &s_dsl_tex;
        if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_pl_tex) != VK_SUCCESS) return vk_die("pl tex");
        pr.size = sizeof(BlitPush); li.pSetLayouts = &s_dsl_blit;
        if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_pl_blit) != VK_SUCCESS) return vk_die("pl blit");
        pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pr.size = sizeof(PackPush); li.pSetLayouts = &s_dsl_pack;
        if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_pl_pack) != VK_SUCCESS) return vk_die("pl pack");
    }

    /* Shader modules (kept for lazy graphics-pipeline creation). */
    s_mod_geo_v  = make_module(spv_geo_vert, spv_geo_vert_size);
    s_mod_geo_f  = make_module(spv_geo_frag, spv_geo_frag_size);
    s_mod_tex_v  = make_module(spv_geo_tex_vert, spv_geo_tex_vert_size);
    s_mod_tex_f  = make_module(spv_geo_tex_frag, spv_geo_tex_frag_size);
    s_mod_blit_v = make_module(spv_blit_vert, spv_blit_vert_size);
    s_mod_blit_f = make_module(spv_blit_frag, spv_blit_frag_size);
    if (!s_mod_geo_v || !s_mod_geo_f || !s_mod_tex_v || !s_mod_tex_f || !s_mod_blit_v || !s_mod_blit_f)
        return vk_die("shader module failed");

    /* Pack compute pipeline. */
    {
        VkShaderModule cm = make_module(spv_pack_comp, spv_pack_comp_size);
        if (!cm) return vk_die("pack module failed");
        VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = cm; ci.stage.pName = "main";
        ci.layout = s_pl_pack;
        VkResult r = p_vkCreateComputePipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &s_pipe_pack);
        p_vkDestroyShaderModule(s_dev, cm, NULL);
        if (r != VK_SUCCESS) return vk_die("pack pipeline failed");
    }

    /* Vertex buffers (untextured + textured). */
    if (!create_buffer_mapped((VkDeviceSize)VK_VBUF_VERTS * sizeof(Vert),
                              &s_vbuf, &s_vbuf_mem, (void**)&s_vmap)) return 0;
    if (!create_buffer_mapped((VkDeviceSize)VK_TBUF_VERTS * sizeof(TexVert),
                              &s_tbuf, &s_tbuf_mem, (void**)&s_tmap)) return 0;
    gpu_sync();   /* complete the init clears (raw) before any frame */
    s_ready = 1;
    /* CPU VRAM writes made before the context existed (facade routed to us
     * since gr_init, s_ready was 0) accumulated only in the CPU array — seed
     * the raw mirror from them. The windowed colour target does not exist yet;
     * its first frame is drawn by the game, and every upload also lands in the
     * raw mirror exactly, so nothing is lost. */
    up_add(0, 0, VRAM_W - 1, VRAM_H - 1);
    flush_cpu_upload();
    return 1;
}

/* Point the pack pass at the current target view (tgt_create calls this). */
static void tgt_set_bindings(void) {
    if (!s_ds_pack || !s_vram_view) return;
    VkDescriptorImageInfo hr_smp = { s_samp, s_vram_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = s_ds_pack; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &hr_smp;
    p_vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);
}

/* Lazily build + cache a graphics pipeline for (program, topology, blend,
 * stencil). prog 0 GEO / 1 TEX / 2 BLIT; topo 0 tri / 1 line; blend 0 off else
 * PS1 mode+1; stencil 0 SET / 1 CHECK-keep / 2 CHECK-invert. */
static VkPipeline get_pipeline(int prog, int topo, int blend, int stencil, int cmask) {
    /* [video] volumetrics needs the depth attachment written (the fog depth
     * proxy rides in clip z). The bit is constant for the process lifetime and
     * costs nothing when the effect is off. */
    int fz = s_vfx_fog ? 1 : 0;
    int key = ((((prog * PIPE_TOPOS + topo) * PIPE_BLENDS + blend) * PIPE_STENCILS + stencil)
              * PIPE_CMASKS + cmask) * PIPE_FOGS + fz;
    if (s_pipe_cache[key]) return s_pipe_cache[key];

    VkShaderModule vs, fs; VkPipelineLayout layout;
    VkVertexInputBindingDescription bind = {0};
    VkVertexInputAttributeDescription attrs[11]; uint32_t nattr;
    if (prog == 0) {            /* GEO */
        vs = s_mod_geo_v; fs = s_mod_geo_f; layout = s_pl_geo;
        bind = (VkVertexInputBindingDescription){ 0, sizeof(Vert), VK_VERTEX_INPUT_RATE_VERTEX };
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
        attrs[1] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8 };
        nattr = 2;
    } else if (prog == 1) {     /* TEX */
        vs = s_mod_tex_v; fs = s_mod_tex_f; layout = s_pl_tex;
        bind = (VkVertexInputBindingDescription){ 0, sizeof(TexVert), VK_VERTEX_INPUT_RATE_VERTEX };
        attrs[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
        attrs[1] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 };
        attrs[2] = (VkVertexInputAttributeDescription){ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
        attrs[3] = (VkVertexInputAttributeDescription){ 3, 0, VK_FORMAT_R32G32_SFLOAT, 32 };
        attrs[4] = (VkVertexInputAttributeDescription){ 4, 0, VK_FORMAT_R32G32_SFLOAT, 40 };
        attrs[5] = (VkVertexInputAttributeDescription){ 5, 0, VK_FORMAT_R32_SFLOAT, 48 };
        attrs[6] = (VkVertexInputAttributeDescription){ 6, 0, VK_FORMAT_R32_SFLOAT, 52 };
        attrs[7] = (VkVertexInputAttributeDescription){ 7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 56 };
        attrs[8] = (VkVertexInputAttributeDescription){ 8, 0, VK_FORMAT_R32_SFLOAT, 72 };  /* a_q */
        attrs[9] = (VkVertexInputAttributeDescription){ 9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 76 }; /* a_hd_source */
        attrs[10] = (VkVertexInputAttributeDescription){ 10, 0, VK_FORMAT_R32_SFLOAT, 92 }; /* a_hd_mode */
        nattr = 11;
    } else {                    /* BLIT (vertex-less: rect from push constant) */
        vs = s_mod_blit_v; fs = s_mod_blit_f; layout = s_pl_blit;
        nattr = 0;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = nattr ? 1 : 0; vin.pVertexBindingDescriptions = nattr ? &bind : NULL;
    vin.vertexAttributeDescriptionCount = nattr; vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* Blend (PS1 modes). Alpha always replaced so the colour-alpha mask mirror
     * is exact (src ONE / dst ZERO). */
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = cmask ? 0 : 0xF;
    float bc[4] = {1,1,1,1};
    if (blend == 0) {
        cba.blendEnable = VK_FALSE;
    } else {
        cba.blendEnable = VK_TRUE;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        switch (blend - 1) {
        case 0: /* B/2 + F/2 */
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            cba.colorBlendOp = VK_BLEND_OP_ADD; bc[0]=bc[1]=bc[2]=bc[3]=0.5f; break;
        case 1: /* B + F */
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.colorBlendOp = VK_BLEND_OP_ADD; break;
        case 2: /* B - F */
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT; break;
        case 3: /* B + F/4 */
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp = VK_BLEND_OP_ADD; bc[0]=bc[1]=bc[2]=bc[3]=0.25f; break;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    cb.blendConstants[0]=bc[0]; cb.blendConstants[1]=bc[1]; cb.blendConstants[2]=bc[2]; cb.blendConstants[3]=bc[3];

    /* Stencil = bit15 mirror (bit0). SET: always+replace, dyn ref. CHECK: test
     * stored==0; write 1 via INVERT (stencil 2) or leave (stencil 1). */
    VkStencilOpState st = {0};
    st.compareMask = 0x01; st.writeMask = 0x01; st.reference = 0;
    if (stencil == 0)      { st.compareOp = VK_COMPARE_OP_ALWAYS; st.failOp = VK_STENCIL_OP_KEEP; st.passOp = VK_STENCIL_OP_REPLACE; st.depthFailOp = VK_STENCIL_OP_KEEP; }
    else if (stencil == 1) { st.compareOp = VK_COMPARE_OP_EQUAL;  st.failOp = VK_STENCIL_OP_KEEP; st.passOp = VK_STENCIL_OP_KEEP;    st.depthFailOp = VK_STENCIL_OP_KEEP; }
    else                   { st.compareOp = VK_COMPARE_OP_EQUAL;  st.failOp = VK_STENCIL_OP_KEEP; st.passOp = VK_STENCIL_OP_INVERT;  st.depthFailOp = VK_STENCIL_OP_KEEP; }
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = fz ? VK_TRUE : VK_FALSE;
    dss.stencilTestEnable = VK_TRUE; dss.front = st; dss.back = st;

    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 3; dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.stageCount = 2; pi.pStages = stages;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp; pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms; pi.pDepthStencilState = &dss; pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy; pi.layout = layout; pi.renderPass = s_rpass;
    VkPipeline p = VK_NULL_HANDLE;
    if (p_vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &pi, NULL, &p) != VK_SUCCESS) {
        vk_die("graphics pipeline create failed"); return VK_NULL_HANDLE;
    }
    s_pipe_cache[key] = p;
    return p;
}

/* PSX 1555 -> RGBA8 (bit15 mask -> alpha). */
static inline void rgb555_to_rgba8(uint16_t p, uint8_t out[4]) {
    out[0] = (uint8_t)(((p)        & 0x1F) << 3);
    out[1] = (uint8_t)(((p >> 5)   & 0x1F) << 3);
    out[2] = (uint8_t)(((p >> 10)  & 0x1F) << 3);
    out[3] = (uint8_t)((p & 0x8000) ? 0xFF : 0x00);
}
static inline uint16_t rgba8_to_rgb555(const uint8_t in[4]) {
    return (uint16_t)(((in[0] >> 3) & 0x1F) |
                      (((in[1] >> 3) & 0x1F) << 5) |
                      (((in[2] >> 3) & 0x1F) << 10) |
                      (in[3] >= 0x80 ? 0x8000 : 0));
}

int vk_renderer_init_context(SDL_Window *win) {
    s_win = win;
#if defined(PSX_SDL3)
    if (!SDL_Vulkan_LoadLibrary(NULL))
#else
    if (SDL_Vulkan_LoadLibrary(NULL) != 0)
#endif
        return vk_die("SDL_Vulkan_LoadLibrary failed (no Vulkan ICD?)");
    p_vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!p_vkGetInstanceProcAddr) return vk_die("no vkGetInstanceProcAddr from SDL");

    if (!load_global_funcs()) return 0;
    if (!create_instance()) return 0;
    if (!load_instance_funcs()) return 0;
#if defined(PSX_SDL3)
    if (!SDL_Vulkan_CreateSurface(s_win, s_instance, NULL, &s_surface))
#else
    if (!SDL_Vulkan_CreateSurface(s_win, s_instance, &s_surface))
#endif
        return vk_die("SDL_Vulkan_CreateSurface failed");
    if (!pick_device()) return 0;
    if (!create_device()) return 0;

    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = s_qfam;
    if (p_vkCreateCommandPool(s_dev, &pi, NULL, &s_cmd_pool) != VK_SUCCESS)
        return vk_die("vkCreateCommandPool failed");
    /* Work pool for one-shot ops, bulk-reset at gpu_sync (TRANSIENT: short-lived
     * CBs). */
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (p_vkCreateCommandPool(s_dev, &pi, NULL, &s_work_pool) != VK_SUCCESS)
        return vk_die("vkCreateCommandPool(work) failed");

    if (!create_swapchain()) return 0;
    if (!create_sync()) return 0;
    /* vk_frame_perf timestamp pool (optional; without device timestamps the
     * command still reports CPU time, allocs, submits and draws). */
    if (s_ts_ok) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = VK_PERF_TS_SLOTS * 5;
        if (p_vkCreateQueryPool(s_dev, &qi, NULL, &s_perf_pool) != VK_SUCCESS) {
            s_ts_ok = 0; s_perf_pool = VK_NULL_HANDLE;
            vk_log("timestamp queries unavailable; vk_frame_perf reports CPU time only");
        }
    } else {
        vk_log("timestamp queries unsupported; vk_frame_perf reports CPU time only");
    }
    /* [video] volumetrics / light shafts (docs/EFFECTS.md): read the config
     * here, arm the flags BEFORE the render targets exist (the fog pass needs
     * its depth-CLEAR variant), and leave every resource allocation to the
     * first armed present. Off (the default) = nothing below changes. */
    vfx_init();
    s_vfx_fog_density    = vfx_fog_density();
    vfx_fog_color(s_vfx_fog_color);
    s_vfx_shaft_strength = vfx_shaft_strength();
    s_vfx_shaft_samples  = vfx_shaft_samples();
    vfx_shaft_light_uv(s_vfx_light_uv);
    s_vfx_fog    = vfx_fog_enabled();
    s_vfx_shafts = vfx_shafts_enabled();

    if (!create_render_targets()) return 0;

    if (s_vfx_fog) {
        /* The fog present samples the depth aspect as the proxy store. Refuse
         * the effect (loudly) on a device that cannot sample that format. */
        VkFormatProperties fp; p_vkGetPhysicalDeviceFormatProperties(s_phys, s_ds_format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            s_vfx_fog = 0;
            fprintf(stdout, "psxrecomp: vfx: fog disabled (depth format not sampleable)\n");
        }
    }
    vfx_set_active(s_vfx_fog, s_vfx_shafts);
    if (s_vfx_fog || s_vfx_shafts)
        fprintf(stdout, "psxrecomp: vfx: armed fog=%d lightshafts=%d\n",
                s_vfx_fog, s_vfx_shafts);
    /* The effects are applied by the PRESENT pass (fog mixes the depth proxy,
     * shafts compose an extra pass). The temporal blend has its own present and
     * would silently drop them, so the effects keep the plain presenter. */
    if (s_vfx_fog || s_vfx_shafts) {
        vk_renderer_set_interpolation_suspended(1);
        vk_log("volumetrics armed: the 60 fps temporal presenter is suspended");
    }
    /* Render target: the default is the whole VRAM (the pre-windowing mode).
     * PSX_VK_TGT=window selects the windowed target for measurement; it is not
     * the default because it loses the frame on the AMD driver (see
     * docs/VULKAN_PARITY.md). */
    {
        const char *tgt = getenv("PSX_VK_TGT");
        const int want_window = tgt && (tgt[0] == 'w' || tgt[0] == 'W');
        s_tgt_full_env = want_window ? 0 : 1;
        if (want_window) vk_log("PSX_VK_TGT=window: windowed render target "
                                "(diagnostic; loses frames on AMD)");
    }

    s_ctx_ok = 1;
    /* HD texture pack: open PSXRECOMP_HD_PACK (a pack already configured by
     * the mods path is reused) and apply the CLUT-row overrides that are
     * already resident in VRAM. */
    if (vk_hd_pack_init()) vk_renderer_hd_pack_changed();
    vk_log("instance/device/swapchain/VRAM + raw mirror/pipelines ready");
    return 1;
}

static void cpres_cache_free(void);   /* fwd: CPU-present resource cache */
static void osd_staging_free(void);
void vk_renderer_shutdown(void) {
    if (!s_dev) return;
    p_vkDeviceWaitIdle(s_dev);
    pp_destroy_all();
    /* [video] volumetrics / light shafts (all NULL when never armed). The
     * swapchain-lifetime half went with pp_destroy_all above. */
    if (s_vfx_fog_pl)    { p_vkDestroyPipelineLayout(s_dev, s_vfx_fog_pl, NULL); s_vfx_fog_pl = VK_NULL_HANDLE; }
    if (s_vfx_shaft_pl)  { p_vkDestroyPipelineLayout(s_dev, s_vfx_shaft_pl, NULL); s_vfx_shaft_pl = VK_NULL_HANDLE; }
    if (s_vfx_pool)      { p_vkDestroyDescriptorPool(s_dev, s_vfx_pool, NULL); s_vfx_pool = VK_NULL_HANDLE; }
    if (s_vfx_fog_dsl)   { p_vkDestroyDescriptorSetLayout(s_dev, s_vfx_fog_dsl, NULL); s_vfx_fog_dsl = VK_NULL_HANDLE; }
    if (s_vfx_shaft_dsl) { p_vkDestroyDescriptorSetLayout(s_dev, s_vfx_shaft_dsl, NULL); s_vfx_shaft_dsl = VK_NULL_HANDLE; }
    if (s_vfx_fog_fs)    { p_vkDestroyShaderModule(s_dev, s_vfx_fog_fs, NULL); s_vfx_fog_fs = VK_NULL_HANDLE; }
    if (s_vfx_shaft_fs)  { p_vkDestroyShaderModule(s_dev, s_vfx_shaft_fs, NULL); s_vfx_shaft_fs = VK_NULL_HANDLE; }
    if (s_vfx_samp)      { p_vkDestroySampler(s_dev, s_vfx_samp, NULL); s_vfx_samp = VK_NULL_HANDLE; }
    vk_gpu_sync_internal();   /* reclaim deferred staging before tearing down */
    cpres_cache_free();       /* FMV CPU-present cached image + staging */
    osd_staging_free();       /* host toast OSD staging */
    for (int i = 0; i < STAGING_CACHE_MAX; ++i) {
        if (s_staging_cache[i].buf)
            staging_destroy(s_staging_cache[i].buf, s_staging_cache[i].mem);
    }
    wide_free_all();          /* native-wide surfaces (color + DS + framebuffers) */
    for (int i = 0; i < PIPE_CACHE_N; i++)
        if (s_pipe_cache[i]) p_vkDestroyPipeline(s_dev, s_pipe_cache[i], NULL);
    if (s_pipe_pack)   p_vkDestroyPipeline(s_dev, s_pipe_pack, NULL);
    if (s_mod_geo_v)   p_vkDestroyShaderModule(s_dev, s_mod_geo_v, NULL);
    if (s_mod_geo_f)   p_vkDestroyShaderModule(s_dev, s_mod_geo_f, NULL);
    if (s_mod_tex_v)   p_vkDestroyShaderModule(s_dev, s_mod_tex_v, NULL);
    if (s_mod_tex_f)   p_vkDestroyShaderModule(s_dev, s_mod_tex_f, NULL);
    if (s_mod_blit_v)  p_vkDestroyShaderModule(s_dev, s_mod_blit_v, NULL);
    if (s_mod_blit_f)  p_vkDestroyShaderModule(s_dev, s_mod_blit_f, NULL);
    if (s_pl_geo)  p_vkDestroyPipelineLayout(s_dev, s_pl_geo, NULL);
    if (s_pl_tex)  p_vkDestroyPipelineLayout(s_dev, s_pl_tex, NULL);
    if (s_pl_blit) p_vkDestroyPipelineLayout(s_dev, s_pl_blit, NULL);
    if (s_pl_pack) p_vkDestroyPipelineLayout(s_dev, s_pl_pack, NULL);
    if (s_dpool)    p_vkDestroyDescriptorPool(s_dev, s_dpool, NULL);
    hd_cache_clear();   /* HD override images (needs s_dev, before the pool) */
    if (s_hd_dpool) { p_vkDestroyDescriptorPool(s_dev, s_hd_dpool, NULL); s_hd_dpool = VK_NULL_HANDLE; }
    if (s_hd_dummy_view) p_vkDestroyImageView(s_dev, s_hd_dummy_view, NULL);
    if (s_hd_dummy_img)  p_vkDestroyImage(s_dev, s_hd_dummy_img, NULL);
    if (s_hd_dummy_mem)  p_vkFreeMemory(s_dev, s_hd_dummy_mem, NULL);
    if (s_perf_pool) { p_vkDestroyQueryPool(s_dev, s_perf_pool, NULL); s_perf_pool = VK_NULL_HANDLE; }
    if (s_dsl_tex)  p_vkDestroyDescriptorSetLayout(s_dev, s_dsl_tex, NULL);
    if (s_dsl_pack) p_vkDestroyDescriptorSetLayout(s_dev, s_dsl_pack, NULL);
    if (s_dsl_blit) p_vkDestroyDescriptorSetLayout(s_dev, s_dsl_blit, NULL);
    if (s_samp)     p_vkDestroySampler(s_dev, s_samp, NULL);
    tgt_destroy();     /* windowed target: colour + depth/stencil + scratch + fbo */
    if (s_rpass)       p_vkDestroyRenderPass(s_dev, s_rpass, NULL);
    if (s_rpass_fog)   p_vkDestroyRenderPass(s_dev, s_rpass_fog, NULL);
    if (s_rpass_fog_load) p_vkDestroyRenderPass(s_dev, s_rpass_fog_load, NULL);
    if (s_raw_view)    p_vkDestroyImageView(s_dev, s_raw_view, NULL);
    if (s_up_view)     p_vkDestroyImageView(s_dev, s_up_view, NULL);
    if (s_vbuf)        p_vkDestroyBuffer(s_dev, s_vbuf, NULL);
    if (s_vbuf_mem)    p_vkFreeMemory(s_dev, s_vbuf_mem, NULL);
    if (s_tbuf)        p_vkDestroyBuffer(s_dev, s_tbuf, NULL);
    if (s_tbuf_mem)    p_vkFreeMemory(s_dev, s_tbuf_mem, NULL);
    if (s_raw_img)     p_vkDestroyImage(s_dev, s_raw_img, NULL);
    if (s_raw_mem)     p_vkFreeMemory(s_dev, s_raw_mem, NULL);
    if (s_up_img)      p_vkDestroyImage(s_dev, s_up_img, NULL);
    if (s_up_mem)      p_vkFreeMemory(s_dev, s_up_mem, NULL);
    for (int i = 0; i < VK_FRAMES; i++) {
        if (s_sem_acquire[i]) p_vkDestroySemaphore(s_dev, s_sem_acquire[i], NULL);
        if (s_sem_render[i])  p_vkDestroySemaphore(s_dev, s_sem_render[i], NULL);
        if (s_fence[i])       p_vkDestroyFence(s_dev, s_fence[i], NULL);
    }
    destroy_swapchain();
    if (s_work_pool) p_vkDestroyCommandPool(s_dev, s_work_pool, NULL);
    if (s_cmd_pool) p_vkDestroyCommandPool(s_dev, s_cmd_pool, NULL);
    p_vkDestroyDevice(s_dev, NULL); s_dev = VK_NULL_HANDLE;
    if (s_surface) p_vkDestroySurfaceKHR(s_instance, s_surface, NULL);
    if (s_instance) p_vkDestroyInstance(s_instance, NULL);
    s_ctx_ok = 0;
}

void vk_renderer_set_present_mode(int mode) { s_present_mode_req = mode; }

/* ---- present ----------------------------------------------------------- */
/* Acquire a swapchain image, run `record` (which leaves it in
 * PRESENT_SRC_KHR-ready state), submit, present. record may be NULL for a
 * blank (the image is cleared to black). Returns 1 on success. */
static int acquire_present(VkImage *out_sc, VkCommandBuffer *out_cb,
                           uint32_t *out_idx, uint32_t *out_frame) {
    uint32_t fr = s_frame_idx % VK_FRAMES;
    p_vkWaitForFences(s_dev, 1, &s_fence[fr], VK_TRUE, UINT64_MAX);
    uint32_t img_idx = 0;
    uint64_t acquire_start = SDL_GetPerformanceCounter();
    VkResult r = p_vkAcquireNextImageKHR(s_dev, s_swapchain, UINT64_MAX,
                                         s_sem_acquire[fr], VK_NULL_HANDLE, &img_idx);
    s_perf_cur.acquire_us += perf_elapsed_us(acquire_start);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        p_vkDeviceWaitIdle(s_dev);
        destroy_swapchain();
        if (!create_swapchain()) return 0;
        return 0;  /* skip this frame; next one uses the new swapchain */
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return 0;
    p_vkResetFences(s_dev, 1, &s_fence[fr]);
    p_vkResetCommandBuffer(s_cmd[fr], 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(s_cmd[fr], &bi);
    /* Present-start stamp. When the frame had no scene work at all, this also
     * writes the scene-start stamp, so scene_gpu reads ~0 rather than stale. */
    perf_ts_scene_begin(s_cmd[fr]);
    if (s_ts_ok)
        p_vkCmdWriteTimestamp(s_cmd[fr], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              s_perf_pool, (s_present_idx % VK_PERF_TS_SLOTS) * 5 + 1);
    *out_sc = s_sc_images[img_idx];
    *out_cb = s_cmd[fr];
    *out_idx = img_idx;
    *out_frame = fr;
    return 1;
}

/* Copy host toast into the swapchain (must still be TRANSFER_DST). Opaque
 * overwrite — host_osd bakes an opaque panel so no blend pass is needed. */
static VkBuffer       s_osd_buf  = VK_NULL_HANDLE;
static VkDeviceMemory s_osd_mem  = VK_NULL_HANDLE;
static void          *s_osd_map  = NULL;
static VkDeviceSize   s_osd_cap  = 0;

static void osd_staging_free(void) {
    if (s_osd_buf) {
        staging_destroy(s_osd_buf, s_osd_mem);
        s_osd_buf = VK_NULL_HANDLE;
        s_osd_mem = VK_NULL_HANDLE;
    }
    s_osd_map = NULL;
    s_osd_cap = 0;
}

static void vk_osd_copy_rect(VkCommandBuffer cb, VkImage sc,
                             const uint32_t *px, int w, int h,
                             int x, int y, VkDeviceSize buf_off) {
    if (!px || w <= 0 || h <= 0) return;
    int dw = w, dh = h;
    if (x + dw > (int)s_sc_extent.width) dw = (int)s_sc_extent.width - x;
    if (y + dh > (int)s_sc_extent.height) dh = (int)s_sc_extent.height - y;
    if (dw < 1 || dh < 1) return;
    memcpy((uint8_t *)s_osd_map + (size_t)buf_off, px,
           (size_t)w * (size_t)h * 4u);
    VkBufferImageCopy bc = {0};
    bc.bufferOffset = buf_off;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageOffset.x = x;
    bc.imageOffset.y = y;
    bc.imageExtent.width = (uint32_t)dw;
    bc.imageExtent.height = (uint32_t)dh;
    bc.imageExtent.depth = 1;
    bc.bufferRowLength = (uint32_t)w;
    bc.bufferImageHeight = (uint32_t)h;
    p_vkCmdCopyBufferToImage(cb, s_osd_buf, sc,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
}

static void vk_osd_blit(VkCommandBuffer cb, VkImage sc) {
    const uint32_t *text_px = NULL, *vol_px = NULL, *rw_px = NULL, *ssm_px = NULL;
    int tw = 0, th = 0, vw = 0, vh = 0, rw = 0, rh = 0, sw = 0, sh = 0;
    const int have_text = host_osd_image(&text_px, &tw, &th) && text_px;
    const int have_vol = host_osd_volume_image(&vol_px, &vw, &vh) && vol_px;
    const int have_rw = psx_rewind_overlay_image(&rw_px, &rw, &rh) && rw_px;
    const int have_ssm =
        psx_savestate_menu_overlay_image(&ssm_px, &sw, &sh) && ssm_px;
    if (!have_text && !have_vol && !have_rw && !have_ssm) {
        host_osd_present_done();
        return;
    }
    /* Only BGRA swapchains match ARGB8888 LE packing used by host_osd. */
    if (s_sc_format != VK_FORMAT_B8G8R8A8_UNORM &&
        s_sc_format != VK_FORMAT_B8G8R8A8_SRGB) {
        host_osd_present_done();
        return;
    }
    VkDeviceSize text_bytes =
        have_text ? (VkDeviceSize)tw * (VkDeviceSize)th * 4u : 0;
    VkDeviceSize vol_bytes =
        have_vol ? (VkDeviceSize)vw * (VkDeviceSize)vh * 4u : 0;
    VkDeviceSize rw_bytes =
        have_rw ? (VkDeviceSize)rw * (VkDeviceSize)rh * 4u : 0;
    VkDeviceSize ssm_bytes =
        have_ssm ? (VkDeviceSize)sw * (VkDeviceSize)sh * 4u : 0;
    VkDeviceSize bytes = text_bytes + vol_bytes + rw_bytes + ssm_bytes;
    if (bytes > s_osd_cap) {
        p_vkQueueWaitIdle(s_queue);
        osd_staging_free();
        if (!make_staging(bytes, &s_osd_buf, &s_osd_mem, &s_osd_map)) {
            host_osd_present_done();
            return;
        }
        s_osd_cap = bytes;
    }
    const int margin = 8;
    VkDeviceSize off = 0;
    if (have_text) {
        vk_osd_copy_rect(cb, sc, text_px, tw, th, margin, margin, off);
        off += text_bytes;
    }
    if (have_vol) {
        int x = ((int)s_sc_extent.width > vw + margin)
                    ? ((int)s_sc_extent.width - vw - margin)
                    : margin;
        int y = ((int)s_sc_extent.height > vh)
                    ? (((int)s_sc_extent.height - vh) / 2)
                    : margin;
        vk_osd_copy_rect(cb, sc, vol_px, vw, vh, x, y, off);
        off += vol_bytes;
    }
    if (have_rw) {
        float slide = psx_rewind_slide();
        int x = ((int)s_sc_extent.width > rw)
                    ? (((int)s_sc_extent.width - rw) / 2)
                    : 0;
        int y = (int)s_sc_extent.height - (int)((float)rh * slide + 0.5f);
        vk_osd_copy_rect(cb, sc, rw_px, rw, rh, x, y, off);
        off += rw_bytes;
    }
    if (have_ssm) {
        int x = ((int)s_sc_extent.width > sw)
                    ? (((int)s_sc_extent.width - sw) / 2)
                    : 0;
        int y = ((int)s_sc_extent.height > sh)
                    ? (((int)s_sc_extent.height - sh) / 2)
                    : 0;
        vk_osd_copy_rect(cb, sc, ssm_px, sw, sh, x, y, off);
    }
    host_osd_present_done();
}

static void submit_present(VkCommandBuffer cb, uint32_t img_idx, uint32_t fr);

static void finish_present(VkCommandBuffer cb, VkImage sc,
                           uint32_t img_idx, uint32_t fr) {
    vk_osd_blit(cb, sc);
    img_barrier(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    submit_present(cb, img_idx, fr);
}

static void submit_present(VkCommandBuffer cb, uint32_t img_idx, uint32_t fr) {
    if (s_ts_ok)
        p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              s_perf_pool, (s_present_idx % VK_PERF_TS_SLOTS) * 5 + 2);
    p_vkEndCommandBuffer(cb);
    /* Present command buffers write the acquired swapchain image with
     * transfer barriers, clears, and blits. Waiting only at color attachment
     * output does not order those operations after presentation-engine
     * release on stricter drivers. */
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT |
                                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s_sem_acquire[fr];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s_sem_render[fr];
    p_vkQueueSubmit(s_queue, 1, &si, s_fence[fr]);

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &s_sem_render[fr];
    pi.swapchainCount = 1; pi.pSwapchains = &s_swapchain;
    pi.pImageIndices = &img_idx;
    uint64_t present_start = SDL_GetPerformanceCounter();
    p_vkQueuePresentKHR(s_queue, &pi);
    s_perf_cur.present_us += perf_elapsed_us(present_start);
    s_frame_idx++;
}

/* Compute a letterbox rect for the swapchain given a source aspect. */
static void letterbox(int sw, int sh, int aw, int ah, VkOffset3D off[2]) {
    /* center-fit aw:ah inside sw x sh */
    int tw = sw, th = sw * ah / aw;
    if (th > sh) { th = sh; tw = sh * aw / ah; }
    int x = (sw - tw) / 2, y = (sh - th) / 2;
    off[0].x = x;       off[0].y = y;       off[0].z = 0;
    off[1].x = x + tw;  off[1].y = y + th;  off[1].z = 1;
}

/* ---- present shot (diagnostic readback) --------------------------------- */
/* present_shot under Vulkan: read the windowed render target back and write the
 * picture rect the present would blit, so the PNG is the PRESENTED IMAGE — same
 * content, same aspect and same internal (SSAA) resolution as the GL capture
 * (which reads its FBO). The earlier version rebuilt the frame from the CPU
 * VRAM mirror at native resolution: it was 15-bit quantised, could be stale,
 * and carried only native details, so `live.shot`'s colour count and the
 * `live.plane` scanout comparison measured the mirror instead of the present.
 *
 * `argb` non-NULL = the already-composed CPU frame (24-bit FMV present); passed
 * as-is because the packed RGB888 bytes have no VRAM coordinates to decode from.
 *
 * Remaining gap vs GL: the OSD toast/rewind/savestate overlays are consumed by
 * vk_osd_blit in the present command buffer, after this readback. */
static uint8_t *vk_target_rgba(int *out_w, int *out_h);   /* below */
static void shot_serve(int dx, int dy, int dw, int dh,
                       const uint32_t *argb, int aspect_num, int aspect_den)
{
    extern int  present_shot_take(char *out, int n);
    extern void present_shot_done(int ok);
    char path[512];
    if (!present_shot_take(path, (int)sizeof(path))) return;

    /* The GPU frame is authoritative for 15-bit presents. NEVER sync the CPU
     * mirror on 24-bit: that readback clobbers the packed RGB888 bytes the CPU
     * present just staged (same rule as main.cpp's depth24 present). */
    uint8_t *tgt = NULL;
    int tw = 0, th = 0;
    if (!argb) tgt = vk_target_rgba(&tw, &th);

    int cw = (int)s_sc_extent.width, ch = (int)s_sc_extent.height;
    if (cw <= 0 || ch <= 0) { cw = dw > 0 ? dw : 1; ch = dh > 0 ? dh : 1; }

    VkOffset3D dst[2];
    letterbox(cw, ch, aspect_num > 0 ? aspect_num : 4,
                      aspect_den > 0 ? aspect_den : 3, dst);
    /* Short GP1(07h) / FMV bands letterbox inside that rect, as the presents do. */
    if (dh > 0 && dh < 240) {
        int box_h = dst[1].y - dst[0].y;
        int content_h = (box_h * dh) / 240;
        if (content_h < 1) content_h = 1;
        dst[0].y += (box_h - content_h) / 2;
        dst[1].y = dst[0].y + content_h;
    }

    /* The PNG is the PRESENTED IMAGE, not the window: write only the picture
     * rect the swapchain blit fills, so its own dimensions are the presented
     * aspect. The GL capture crops the same way (present_snapshot), and the
     * acceptance row `live.present_aspect` reads these dimensions. Writing the
     * full window here made a correctly letterboxed 341:192 present report the
     * window's 4:3 forever (480x360 instead of 480x270). */
    int pw = dst[1].x - dst[0].x;
    int ph = dst[1].y - dst[0].y;
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    uint8_t *rgb = (uint8_t *)calloc((size_t)pw * (size_t)ph * 3u, 1u);
    if (!rgb) { free(tgt); present_shot_done(0); return; }

    GpuDisplayInfo di;
    memset(&di, 0, sizeof di);
    di.display_x = (uint32_t)(dx < 0 ? 0 : dx);
    di.display_y = (uint32_t)(dy < 0 ? 0 : dy);

    if (dw > 0 && dh > 0) {
        const int S = s_scale;
        for (int y = 0; y < ph; y++) {
            uint32_t sy = (uint32_t)((int64_t)y * dh / ph);
            uint8_t *row = rgb + (size_t)y * (size_t)pw * 3u;
            for (int x = 0; x < pw; x++, row += 3) {
                uint32_t sx = (uint32_t)((int64_t)x * dw / pw);
                if (argb) {
                    uint32_t p = argb[(size_t)sy * (size_t)dw + sx];
                    row[0] = (uint8_t)(p >> 16);
                    row[1] = (uint8_t)(p >> 8);
                    row[2] = (uint8_t)p;
                } else if (tgt && sx + (uint32_t)dx < (uint32_t)s_tw &&
                           sy + (uint32_t)dy < (uint32_t)s_th) {
                    const size_t tx = (size_t)((sx + (uint32_t)dx) * (uint32_t)S);
                    const size_t ty = (size_t)((sy + (uint32_t)dy) * (uint32_t)S);
                    const uint8_t *p = tgt + (ty * (size_t)tw + tx) * 4u;
                    row[0] = p[0]; row[1] = p[1]; row[2] = p[2];
                } else {
                    /* No target (boot / init failure): native VRAM decode. */
                    gpu_display_pixel_rgb(&di, sx, sy, &row[0], &row[1], &row[2]);
                }
            }
        }
    }

    int wrote = 0;
    FILE *pf = fopen(path, "wb");
    if (pf) { wrote = png_write_rgb(pf, rgb, (uint32_t)pw, (uint32_t)ph); fclose(pf); }
    free(rgb);
    free(tgt);
    present_shot_done(wrote);
}

/* Copy the windowed target down to RGBA8 (target texels: s_tw*S x s_th*S).
 * Caller frees. Used by the present_shot readback and the diagnostic dump. */
static uint8_t *vk_target_rgba(int *out_w, int *out_h) {
    if (!s_tgt_ready || !s_vram_img) return NULL;
    int S = s_scale, w = s_tw, h = s_th;
    if (w <= 0 || h <= 0) return NULL;
    size_t bytes = (size_t)w * S * (size_t)h * S * 4u;
    VkBuffer buf; VkDeviceMemory mem; void *map;
    if (!make_staging((VkDeviceSize)bytes, &buf, &mem, &map)) return NULL;
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_vram_img, &s_vram_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rc = {0};
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rc.imageSubresource.layerCount = 1;
    rc.imageExtent.width = (uint32_t)(w * S);
    rc.imageExtent.height = (uint32_t)(h * S);
    rc.imageExtent.depth = 1;
    rc.bufferRowLength = (uint32_t)(w * S);
    p_vkCmdCopyImageToBuffer(cb, s_vram_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rc);
    end_oneshot(cb);
    gpu_sync();
    uint8_t *rgba = (uint8_t *)malloc(bytes);
    if (rgba) memcpy(rgba, map, bytes);
    free_staging(buf, mem);
    if (rgba) { if (out_w) *out_w = w * S; if (out_h) *out_h = h * S; }
    return rgba;
}

/* PSX_VK_DUMP_TGT=<png>: one-shot diagnostic dump of the windowed render
 * target, at native resolution. Used to tell "the GPU never drew it" from
 * "the present/pack lost it" — no other readback exposes the target. */
static void vk_dump_target(const char *path) {
    int tw = 0, th = 0;
    uint8_t *src = vk_target_rgba(&tw, &th);
    if (!src) return;
    int S = s_scale, w = tw / (S > 0 ? S : 1), h = th / (S > 0 ? S : 1);
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3u);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = src + (((size_t)y * S * (size_t)tw) + (size_t)x * S) * 4u;
            uint8_t *o = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
        }
    FILE *pf = fopen(path, "wb");
    if (pf) { png_write_rgb(pf, rgb, (uint32_t)w, (uint32_t)h); fclose(pf); }
    free(rgb); free(src);
    fprintf(stdout, "psxrecomp: vulkan: target dump %s (%dx%d, target origin %d,%d)\n",
            path, w, h, s_tx0, s_ty0);
}

/* ---- opaque swapchain present pass --------------------------------------- */
/* The plain present path blitted the VRAM image into the swapchain with
 * vkCmdBlitImage, which copies alpha too. VRAM alpha is the PSX mask/STP bit
 * (0 on most pixels), and AMD's compositor honours it despite
 * VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR: parts of the window turned transparent
 * (black/white blocks of whatever is behind it). This pass draws the displayed
 * rect with a fullscreen triangle whose fragment shader writes alpha = 1.
 * Everything is created lazily on first use; any failure sets s_pp_failed and
 * the callers fall back to the original blit. The render pass ends in
 * TRANSFER_DST_OPTIMAL so finish_present() (OSD blit + present barrier) runs
 * unchanged. Format-dependent objects are released with the swapchain. */
static VkRenderPass          s_pp_rp;
static VkImageView           s_pp_views[8];
static VkFramebuffer         s_pp_fbs[8];
static VkPipeline            s_pp_pipe;
static VkDescriptorSetLayout s_pp_dsl;
static VkPipelineLayout      s_pp_pl;
static VkDescriptorPool      s_pp_pool;
static VkDescriptorSet       s_pp_ds[VK_FRAMES];
static VkShaderModule        s_pp_vs, s_pp_fs;
static VkSampler             s_pp_near, s_pp_lin;
static int                   s_pp_failed;

typedef struct { float src_rect[4]; } PresentPush;

/* Swapchain-lifetime objects (the render pass depends on s_sc_format). */
static void pp_release_swapchain(void) {
    for (int i = 0; i < 8; i++) {
        if (s_pp_fbs[i])   { p_vkDestroyFramebuffer(s_dev, s_pp_fbs[i], NULL); s_pp_fbs[i] = VK_NULL_HANDLE; }
        if (s_pp_views[i]) { p_vkDestroyImageView(s_dev, s_pp_views[i], NULL); s_pp_views[i] = VK_NULL_HANDLE; }
    }
    if (s_pp_pipe) { p_vkDestroyPipeline(s_dev, s_pp_pipe, NULL); s_pp_pipe = VK_NULL_HANDLE; }
    if (s_pp_rp)   { p_vkDestroyRenderPass(s_dev, s_pp_rp, NULL); s_pp_rp = VK_NULL_HANDLE; }
    vfx_release_swapchain();   /* post image + fog/shaft pipelines are format-bound */
    ri_release_swapchain();    /* temporal-blend pipeline is render-pass-bound */
}

static void pp_destroy_all(void) {
    pp_release_swapchain();
    if (s_pp_pool) { p_vkDestroyDescriptorPool(s_dev, s_pp_pool, NULL); s_pp_pool = VK_NULL_HANDLE; }
    if (s_pp_pl)   { p_vkDestroyPipelineLayout(s_dev, s_pp_pl, NULL); s_pp_pl = VK_NULL_HANDLE; }
    if (s_pp_dsl)  { p_vkDestroyDescriptorSetLayout(s_dev, s_pp_dsl, NULL); s_pp_dsl = VK_NULL_HANDLE; }
    if (s_pp_vs)   { p_vkDestroyShaderModule(s_dev, s_pp_vs, NULL); s_pp_vs = VK_NULL_HANDLE; }
    if (s_pp_fs)   { p_vkDestroyShaderModule(s_dev, s_pp_fs, NULL); s_pp_fs = VK_NULL_HANDLE; }
    if (s_pp_near) { p_vkDestroySampler(s_dev, s_pp_near, NULL); s_pp_near = VK_NULL_HANDLE; }
    if (s_pp_lin)  { p_vkDestroySampler(s_dev, s_pp_lin, NULL); s_pp_lin = VK_NULL_HANDLE; }
    ri_destroy_all();   /* temporal-blend images/sets/layouts/shaders */
    s_pp_failed = 0;
}

/* Device-lifetime objects: shaders, samplers, set layout, pool, sets, layout. */
static int pp_init_device_objects(void) {
    if (s_pp_pl) return 1;
    s_pp_vs = make_module(spv_present_vert, spv_present_vert_size);
    s_pp_fs = make_module(spv_present_frag, spv_present_frag_size);
    if (!s_pp_vs || !s_pp_fs) return 0;

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (p_vkCreateSampler(s_dev, &sci, NULL, &s_pp_near) != VK_SUCCESS) return 0;
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    if (p_vkCreateSampler(s_dev, &sci, NULL, &s_pp_lin) != VK_SUCCESS) return 0;

    VkDescriptorSetLayoutBinding b = {0};
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dci.bindingCount = 1; dci.pBindings = &b;
    if (p_vkCreateDescriptorSetLayout(s_dev, &dci, NULL, &s_pp_dsl) != VK_SUCCESS) return 0;

    /* One set per in-flight frame: acquire_present waits the frame fence, so
     * the set is idle when it is rewritten for the next present. */
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_FRAMES };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = VK_FRAMES; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
    if (p_vkCreateDescriptorPool(s_dev, &pci, NULL, &s_pp_pool) != VK_SUCCESS) return 0;
    for (int i = 0; i < VK_FRAMES; i++) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = s_pp_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &s_pp_dsl;
        if (p_vkAllocateDescriptorSets(s_dev, &ai, &s_pp_ds[i]) != VK_SUCCESS) return 0;
    }

    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PresentPush) };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.setLayoutCount = 1; li.pSetLayouts = &s_pp_dsl;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    return p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_pp_pl) == VK_SUCCESS;
}

static int pp_init_swapchain_objects(void) {
    if (s_pp_pipe) return 1;
    VkAttachmentDescription att = {0};
    att.format = s_sc_format; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;          /* black letterbox bars */
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;   /* finish_present */
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
    VkSubpassDependency dep[2] = {0};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    /* The OSD blit in finish_present writes the image with transfer ops. */
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2; rpi.pDependencies = dep;
    if (p_vkCreateRenderPass(s_dev, &rpi, NULL, &s_pp_rp) != VK_SUCCESS) return 0;

    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = s_pp_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s_pp_fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {0};
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2; ci.pStages = st;
    ci.pVertexInputState = &vin; ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp; ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms; ci.pColorBlendState = &cb; ci.pDynamicState = &dy;
    ci.layout = s_pp_pl; ci.renderPass = s_pp_rp;
    return p_vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &s_pp_pipe) == VK_SUCCESS;
}

/* Framebuffer for swapchain image idx, created on first use. */
static VkFramebuffer pp_framebuffer(uint32_t idx) {
    if (idx >= 8 || idx >= s_sc_count) return VK_NULL_HANDLE;
    if (s_pp_fbs[idx]) return s_pp_fbs[idx];
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = s_sc_images[idx]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = s_sc_format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    if (p_vkCreateImageView(s_dev, &vi, NULL, &s_pp_views[idx]) != VK_SUCCESS) {
        s_pp_views[idx] = VK_NULL_HANDLE; return VK_NULL_HANDLE;
    }
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = s_pp_rp; fi.attachmentCount = 1; fi.pAttachments = &s_pp_views[idx];
    fi.width = s_sc_extent.width; fi.height = s_sc_extent.height; fi.layers = 1;
    if (p_vkCreateFramebuffer(s_dev, &fi, NULL, &s_pp_fbs[idx]) != VK_SUCCESS)
        s_pp_fbs[idx] = VK_NULL_HANDLE;
    return s_pp_fbs[idx];
}

/* Record the opaque present of src_view (already SHADER_READ_ONLY_OPTIMAL,
 * normalized src rect u0..v1) into the letterbox rect dst of the acquired
 * swapchain image, leaving it in TRANSFER_DST_OPTIMAL. Returns 0 (and records
 * nothing) when the pass is unavailable: the caller then uses the blit. */
static int pp_draw(VkCommandBuffer cb, uint32_t idx, uint32_t fr, VkImageView src_view,
                   int linear, const VkOffset3D dst[2],
                   float u0, float v0, float u1, float v1) {
    if (s_pp_failed) return 0;
    if (!pp_init_device_objects() || !pp_init_swapchain_objects()) {
        s_pp_failed = 1;
        fprintf(stdout, "psxrecomp: vulkan: present pass unavailable, using blit\n");
        return 0;
    }
    VkFramebuffer fb = pp_framebuffer(idx);
    if (!fb) return 0;

    VkDescriptorImageInfo ii = { linear ? s_pp_lin : s_pp_near, src_view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = s_pp_ds[fr]; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    p_vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);

    VkClearValue clear = {0};
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = s_pp_rp; bi.framebuffer = fb;
    bi.renderArea.extent = s_sc_extent;
    bi.clearValueCount = 1; bi.pClearValues = &clear;
    p_vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport view = { (float)dst[0].x, (float)dst[0].y,
                        (float)(dst[1].x - dst[0].x), (float)(dst[1].y - dst[0].y), 0.0f, 1.0f };
    VkRect2D sci = { { dst[0].x, dst[0].y },
                     { (uint32_t)(dst[1].x - dst[0].x), (uint32_t)(dst[1].y - dst[0].y) } };
    p_vkCmdSetViewport(cb, 0, 1, &view);
    p_vkCmdSetScissor(cb, 0, 1, &sci);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pp_pipe);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pp_pl, 0, 1, &s_pp_ds[fr], 0, NULL);
    PresentPush pc = { { u0, v0, u1, v1 } };
    p_vkCmdPushConstants(cb, s_pp_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    perf_draw(cb, 3, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);
    return 1;
}

/* ---- [video] volumetrics / light shafts present chain -------------------
 * docs/EFFECTS.md. Two independent stages, both behind the config and both
 * operating on the PRESENTED picture only (guest VRAM is never read as an
 * effect source beyond the normal present sample, and never written):
 *
 *   fog    present_fog.frag - the plain present sample plus a mix toward the
 *          fog colour driven by the depth attachment, which the geometry pass
 *          used as a per-primitive distance-proxy store. No extra pass: the
 *          same fullscreen triangle just reads one more texture.
 *   shafts lightshaft.frag  - the picture is composed into an offscreen post
 *          image first, then a radial blur of its bright pixels is added on
 *          the way to the swapchain (one extra fullscreen pass + N taps).
 *
 * Everything here is lazily created on the first armed present; with
 * volumetrics = "off" none of it is ever touched. The resource handles and
 * VfxPush are declared with the config block at the top of the file so the
 * teardown path can release them. */
static VkDescriptorSet       s_vfx_fog_ds[VK_FRAMES];
static VkDescriptorSet       s_vfx_shaft_ds[VK_FRAMES];

/* Depth aspect of the canonical target: attachment layout by default, sampled
 * only while a fog present reads the proxy. */
static void vfx_ds_to(VkCommandBuffer cb, VkImageLayout want) {
    if (!s_ds_img || s_vfx_ds_layout == want) return;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = s_vfx_ds_layout; b.newLayout = want;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s_ds_img;
    b.subresourceRange.aspectMask = DS_ASPECT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    if (want == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
        b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        p_vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    } else {
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0, 0, NULL, 0, NULL, 1, &b);
    }
    s_vfx_ds_layout = want;
}

/* Device-lifetime VFX objects: shaders, sampler, set layouts, pool, sets,
 * pipeline layouts. */
static int vfx_init_device(void) {
    if (s_vfx_fog_pl) return 1;
    s_vfx_fog_fs   = make_module(spv_present_fog_frag, spv_present_fog_frag_size);
    s_vfx_shaft_fs = make_module(spv_lightshaft_frag, spv_lightshaft_frag_size);
    if (!s_vfx_fog_fs || !s_vfx_shaft_fs) return 0;

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (p_vkCreateSampler(s_dev, &sci, NULL, &s_vfx_samp) != VK_SUCCESS) return 0;

    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dci.bindingCount = 2; dci.pBindings = b;
    if (p_vkCreateDescriptorSetLayout(s_dev, &dci, NULL, &s_vfx_fog_dsl) != VK_SUCCESS) return 0;
    dci.bindingCount = 1;
    if (p_vkCreateDescriptorSetLayout(s_dev, &dci, NULL, &s_vfx_shaft_dsl) != VK_SUCCESS) return 0;

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * VK_FRAMES };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 2 * VK_FRAMES; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
    if (p_vkCreateDescriptorPool(s_dev, &pci, NULL, &s_vfx_pool) != VK_SUCCESS) return 0;
    for (int i = 0; i < VK_FRAMES; i++) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = s_vfx_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &s_vfx_fog_dsl;
        if (p_vkAllocateDescriptorSets(s_dev, &ai, &s_vfx_fog_ds[i]) != VK_SUCCESS) return 0;
        ai.pSetLayouts = &s_vfx_shaft_dsl;
        if (p_vkAllocateDescriptorSets(s_dev, &ai, &s_vfx_shaft_ds[i]) != VK_SUCCESS) return 0;
    }

    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VfxPush) };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.setLayoutCount = 1; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    li.pSetLayouts = &s_vfx_fog_dsl;
    if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_vfx_fog_pl) != VK_SUCCESS) return 0;
    li.pSetLayouts = &s_vfx_shaft_dsl;
    if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_vfx_shaft_pl) != VK_SUCCESS) return 0;
    return 1;
}

/* Post image (swapchain-sized, swapchain format) and its framebuffer. */
static void vfx_release_post(void) {
    if (s_vfx_post_fb)   { p_vkDestroyFramebuffer(s_dev, s_vfx_post_fb, NULL); s_vfx_post_fb = VK_NULL_HANDLE; }
    if (s_vfx_post_view) { p_vkDestroyImageView(s_dev, s_vfx_post_view, NULL); s_vfx_post_view = VK_NULL_HANDLE; }
    if (s_vfx_post_img)  { p_vkDestroyImage(s_dev, s_vfx_post_img, NULL); s_vfx_post_img = VK_NULL_HANDLE; }
    if (s_vfx_post_mem)  { p_vkFreeMemory(s_dev, s_vfx_post_mem, NULL); s_vfx_post_mem = VK_NULL_HANDLE; }
    s_vfx_post_w = s_vfx_post_h = 0;
}

/* Swapchain-lifetime VFX objects: the post image, its render pass and the two
 * fullscreen pipelines are all bound to the swapchain format, so they go with
 * it (pp_release_swapchain calls this). */
static void vfx_release_swapchain(void) {
    vfx_release_post();
    if (s_vfx_post_rp)    { p_vkDestroyRenderPass(s_dev, s_vfx_post_rp, NULL); s_vfx_post_rp = VK_NULL_HANDLE; }
    if (s_vfx_fog_pipe)   { p_vkDestroyPipeline(s_dev, s_vfx_fog_pipe, NULL); s_vfx_fog_pipe = VK_NULL_HANDLE; }
    if (s_vfx_shaft_pipe) { p_vkDestroyPipeline(s_dev, s_vfx_shaft_pipe, NULL); s_vfx_shaft_pipe = VK_NULL_HANDLE; }
}

/* Lazily build the post pass, its framebuffer and the two fullscreen
 * pipelines (format-dependent). Render-pass compatible with s_pp_rp, so the
 * plain present pipeline can compose into the post image. */
static int vfx_init_swapchain(void) {
    if (!pp_init_device_objects() || !pp_init_swapchain_objects()) return 0;

    if (!s_vfx_post_rp && s_vfx_shafts) {
        VkAttachmentDescription att = {0};
        att.format = s_sc_format; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;      /* letterbox bars stay black */
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {0};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
        /* Make the composed picture visible to the light-shaft pass. */
        VkSubpassDependency dep = {0};
        dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpi.attachmentCount = 1; rpi.pAttachments = &att;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
        rpi.dependencyCount = 1; rpi.pDependencies = &dep;
        if (p_vkCreateRenderPass(s_dev, &rpi, NULL, &s_vfx_post_rp) != VK_SUCCESS) return 0;
    }

    if (!s_vfx_fog_pipe || !s_vfx_shaft_pipe) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba = {0};
        ba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &ba;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = s_pp_vs; st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].pName = "main";
        ci.stageCount = 2; ci.pStages = st;
        ci.pVertexInputState = &vin; ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp; ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms; ci.pColorBlendState = &cb; ci.pDynamicState = &dy;
        ci.renderPass = s_pp_rp;
        st[1].module = s_vfx_fog_fs; ci.layout = s_vfx_fog_pl;
        if (p_vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &s_vfx_fog_pipe) != VK_SUCCESS)
            return 0;
        st[1].module = s_vfx_shaft_fs; ci.layout = s_vfx_shaft_pl;
        if (p_vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &s_vfx_shaft_pipe) != VK_SUCCESS)
            return 0;
    }

    /* The post image is the light-shaft intermediate; fog-only needs none. */
    if (!s_vfx_shafts) return 1;
    if (s_vfx_post_fb && s_vfx_post_w == s_sc_extent.width && s_vfx_post_h == s_sc_extent.height)
        return 1;
    p_vkQueueWaitIdle(s_queue);            /* the old post image may be in flight */
    vfx_release_post();
    if (!make_image(s_sc_format, (int)s_sc_extent.width, (int)s_sc_extent.height,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_vfx_post_img, &s_vfx_post_mem,
                    &s_vfx_post_view)) return 0;
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = s_vfx_post_rp; fi.attachmentCount = 1; fi.pAttachments = &s_vfx_post_view;
    fi.width = s_sc_extent.width; fi.height = s_sc_extent.height; fi.layers = 1;
    if (p_vkCreateFramebuffer(s_dev, &fi, NULL, &s_vfx_post_fb) != VK_SUCCESS) {
        vfx_release_post(); return 0;
    }
    s_vfx_post_w = s_sc_extent.width; s_vfx_post_h = s_sc_extent.height;
    return 1;
}

/* One fullscreen triangle into `fb` with `pipe`, push constants and an optional
 * depth-proxy attachment read. Leaves the target in the pass's finalLayout. */
static void vfx_draw_pass(VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb,
                          VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSet set,
                          const VkOffset3D dst[2], const VfxPush *pc,
                          VkExtent2D area) {
    VkClearValue clear = {0};
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = rp; bi.framebuffer = fb; bi.renderArea.extent = area;
    bi.clearValueCount = 1; bi.pClearValues = &clear;
    p_vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport view = { (float)dst[0].x, (float)dst[0].y,
                        (float)(dst[1].x - dst[0].x), (float)(dst[1].y - dst[0].y), 0.0f, 1.0f };
    VkRect2D sci = { { dst[0].x, dst[0].y },
                     { (uint32_t)(dst[1].x - dst[0].x), (uint32_t)(dst[1].y - dst[0].y) } };
    p_vkCmdSetViewport(cb, 0, 1, &view);
    p_vkCmdSetScissor(cb, 0, 1, &sci);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
    p_vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *pc, pc);
    perf_draw(cb, 3, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);
}

/* Record the present of src_view through the armed effects into the acquired
 * swapchain image `idx`, leaving it in TRANSFER_DST_OPTIMAL (finish_present
 * contract). Returns 0 when nothing was recorded, so the caller keeps its
 * plain present path. */
static int vfx_present_draw(VkCommandBuffer cb, uint32_t idx, uint32_t fr,
                            VkImageView src_view, int linear, const VkOffset3D dst[2],
                            float u0, float v0, float u1, float v1) {
    if ((!s_vfx_fog && !s_vfx_shafts) || s_vfx_failed) return 0;
    /* The effect pipelines build against the swapchain present render pass and
     * its framebuffers (s_pp_rp / pp_framebuffer). With the effects armed this
     * function REPLACES pp_draw, so nothing has created them yet - the first
     * frame then failed every pipeline, latched s_vfx_failed and silently fell
     * back to the plain blit (armed effects, zero pixels changed, vfx_ms 0).
     * Create the present-pass objects first, exactly like pp_draw does. */
    if (!pp_init_device_objects() || !pp_init_swapchain_objects()) return 0;
    if (!vfx_init_device() || !vfx_init_swapchain()) {
        s_vfx_failed = 1;
        fprintf(stdout, "psxrecomp: vfx: present chain unavailable, using the plain present\n");
        return 0;
    }
    VkFramebuffer fb = pp_framebuffer(idx);
    if (!fb) return 0;

    if (s_ts_ok)
        p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              s_perf_pool, (s_present_idx % VK_PERF_TS_SLOTS) * 5 + 3);
    s_perf_cur.has_vfx = 1;

    VfxPush pc = {0};
    pc.src_rect[0] = u0; pc.src_rect[1] = v0; pc.src_rect[2] = u1; pc.src_rect[3] = v1;
    VkSampler src_samp = linear ? s_pp_lin : s_pp_near;

    /* Descriptor set of the compose stage: the plain present binds only the
     * picture, the fog present also binds the depth proxy. */
    VkDescriptorSet compose_set = s_pp_ds[fr];
    VkPipeline      compose_pipe = s_pp_pipe;
    VkPipelineLayout compose_pl = s_pp_pl;
    if (s_vfx_fog) {
        vfx_ds_to(cb, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        VkDescriptorImageInfo ii[2] = {
            { src_samp, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { s_vfx_samp, s_ds_sample_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL } };
        VkWriteDescriptorSet w[2] = {0};
        for (int i = 0; i < 2; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s_vfx_fog_ds[fr]; w[i].dstBinding = (uint32_t)i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &ii[i];
        }
        p_vkUpdateDescriptorSets(s_dev, 2, w, 0, NULL);
        compose_set = s_vfx_fog_ds[fr];
        compose_pipe = s_vfx_fog_pipe;
        compose_pl = s_vfx_fog_pl;
        pc.fog[0] = s_vfx_fog_color[0];
        pc.fog[1] = s_vfx_fog_color[1];
        pc.fog[2] = s_vfx_fog_color[2];
        pc.fog[3] = s_vfx_fog_density;
    } else {
        VkDescriptorImageInfo ii = { src_samp, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = s_pp_ds[fr]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
        p_vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);
    }

    if (!s_vfx_shafts) {
        /* FOG ONLY: the plain present triangle with one extra texture read. */
        vfx_draw_pass(cb, s_pp_rp, fb, compose_pipe, compose_pl, compose_set,
                      dst, &pc, s_sc_extent);
        vfx_ds_to(cb, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    } else {
        /* SHAFTS (with or without fog): compose into the post image, then
         * radial-blur the bright pixels of that picture onto the swapchain. */
        vfx_draw_pass(cb, s_vfx_post_rp, s_vfx_post_fb, compose_pipe, compose_pl,
                      compose_set, dst, &pc, s_sc_extent);
        if (s_vfx_fog) vfx_ds_to(cb, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        VkDescriptorImageInfo ii = { s_pp_lin, s_vfx_post_view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = s_vfx_shaft_ds[fr]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
        p_vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);

        VfxPush sp = {0};
        /* Picture rect inside the post image (the letterboxed dst). */
        sp.src_rect[0] = (float)dst[0].x / (float)s_sc_extent.width;
        sp.src_rect[1] = (float)dst[0].y / (float)s_sc_extent.height;
        sp.src_rect[2] = (float)dst[1].x / (float)s_sc_extent.width;
        sp.src_rect[3] = (float)dst[1].y / (float)s_sc_extent.height;
        sp.fog[0] = s_vfx_shaft_strength;
        sp.fog[1] = (float)s_vfx_shaft_samples;
        sp.fog[2] = s_vfx_light_uv[0];
        sp.fog[3] = s_vfx_light_uv[1];
        vfx_draw_pass(cb, s_pp_rp, fb, s_vfx_shaft_pipe, s_vfx_shaft_pl,
                      s_vfx_shaft_ds[fr], dst, &sp, s_sc_extent);
    }

    if (s_ts_ok)
        p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              s_perf_pool, (s_present_idx % VK_PERF_TS_SLOTS) * 5 + 4);
    return 1;
}

/* ---- temporal 60 fps presenter (the GL interp twin) ----------------------
 * One capture of the displayed image per guest flip into a ping-pong history
 * image, then N presents per guest interval: each present crossfades the two
 * newest captures at the schedule's alpha. Guest logic, timers and audio are
 * untouched - this is presentation time only. Mirrors gpu_gl_renderer.c:
 *   captures   interp_capture            -> ri_capture
 *   schedule   interp_present_source_interval -> ri_present_source_interval
 *   blend      interp_draw_textures/INTERP_FS -> ri_present/interp.frag
 *   cadence    gl_renderer_interpolation_owns_cadence -> vk_..._owns_cadence
 */
#define VK_RI_IMGS 3
static VkImage        s_ri_img[VK_RI_IMGS];
static VkDeviceMemory s_ri_mem[VK_RI_IMGS];
static VkImageView    s_ri_view[VK_RI_IMGS];
static VkImageLayout  s_ri_layout[VK_RI_IMGS];
static int s_ri_alloc_w, s_ri_alloc_h;
static VkShaderModule s_ri_fs;
static VkDescriptorSetLayout s_ri_dsl;
static VkDescriptorPool s_ri_pool;
static VkDescriptorSet  s_ri_ds[VK_FRAMES];
static VkPipelineLayout s_ri_pl;
static VkPipeline       s_ri_pipe;
typedef struct { float alpha; int mode; } RiPush;

static int    s_ri_enabled, s_ri_suspended, s_ri_blend, s_ri_hold, s_ri_source;
static double s_ri_host_hz, s_ri_target_hz, s_ri_source_hz;
static FrameInterpolationSchedule s_ri_sched;
static FrameFlipTracker s_ri_flip;
static int    s_ri_valid, s_ri_cur, s_ri_prev;
static int    s_ri_w, s_ri_h;             /* history image (capture) size */
static int    s_ri_src_w, s_ri_src_h;     /* source band at internal scale */
static int    s_ri_force43, s_ri_linear, s_ri_path;
static int    s_ri_ox = -1, s_ri_oy = -1;
static double s_ri_lo = 0.0, s_ri_hi = 1.0;
static uint64_t s_ri_captures, s_ri_swaps, s_ri_dups;
static int    s_ri_logged;

static void ri_release_swapchain(void);
static void ri_destroy_all(void);

static int ri_active(void) {
    /* The [video] volumetrics passes ARE the present (fog mixes the depth
     * proxy; shafts compose a second fullscreen pass). The temporal blend has
     * its own present and would silently drop them, so an armed effect set
     * keeps the plain presenter - the same rule as the FMV suspension, but
     * keyed on the config instead of a per-frame classification. */
    return s_ctx_ok && s_ri_enabled && !s_ri_suspended &&
           !(s_vfx_fog || s_vfx_shafts);
}

static void ri_invalidate(void) {
    s_ri_valid = 0;
    s_ri_cur = s_ri_prev = 0;
    s_ri_src_w = s_ri_src_h = 0;
    s_ri_path = -1;
    s_ri_force43 = 0;
    s_ri_ox = s_ri_oy = -1;
    s_ri_lo = 0.0; s_ri_hi = 1.0;
    frame_interpolation_schedule_reset(&s_ri_sched);
    frame_flip_tracker_reset(&s_ri_flip);
}

static void ri_free_images(void) {
    for (int i = 0; i < VK_RI_IMGS; i++) {
        if (s_ri_view[i]) p_vkDestroyImageView(s_dev, s_ri_view[i], NULL);
        if (s_ri_img[i])  p_vkDestroyImage(s_dev, s_ri_img[i], NULL);
        if (s_ri_mem[i])  p_vkFreeMemory(s_dev, s_ri_mem[i], NULL);
        s_ri_view[i] = VK_NULL_HANDLE; s_ri_img[i] = VK_NULL_HANDLE;
        s_ri_mem[i] = VK_NULL_HANDLE; s_ri_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    s_ri_alloc_w = s_ri_alloc_h = 0;
    s_ri_w = s_ri_h = 0;
    s_ri_valid = 0;
}

/* Capture size: the letterbox rect when it is smaller than the source band
 * (one filtered blit), exactly like the GL capture_size(). At S=9 a 682-px
 * display is 6138x2160 while the window shows it in ~2784x1566; capturing the
 * band costs 2.5x the traffic for pixels nothing displays. */
static void ri_capture_size(int sw, int sh, int force43, int *cw, int *ch) {
    *cw = sw; *ch = sh;
    if (s_sc_extent.width < 1 || s_sc_extent.height < 1) return;
    VkOffset3D d[2];
    letterbox((int)s_sc_extent.width, (int)s_sc_extent.height,
              force43 ? 4 : s_aspect_num, force43 ? 3 : s_aspect_den, d);
    int lw = d[1].x - d[0].x, lh = d[1].y - d[0].y;
    if (lw > 0 && lh > 0 && (int64_t)lw * lh < (int64_t)sw * sh) { *cw = lw; *ch = lh; }
}

static int ri_ensure(int w, int h) {
    if (s_ri_alloc_w == w && s_ri_alloc_h == h && s_ri_img[0]) return 1;
    ri_free_images();
    if (w < 1 || h < 1) return 0;
    for (int i = 0; i < VK_RI_IMGS; i++) {
        if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, w, h,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT,
                        &s_ri_img[i], &s_ri_mem[i], &s_ri_view[i])) {
            ri_free_images();
            return 0;
        }
        s_ri_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    s_ri_alloc_w = w; s_ri_alloc_h = h;
    s_ri_valid = 0;
    return 1;
}

/* Sampleable after the capture; written again on the next capture, so the
 * write barrier comes from SHADER_READ (the previous blend read it). */
static void ri_layout_to(VkCommandBuffer cb, int i, VkImageLayout want) {
    if (s_ri_layout[i] == want) return;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = s_ri_layout[i]; b.newLayout = want;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s_ri_img[i];
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    VkPipelineStageFlags ss = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags ds = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (want == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        if (s_ri_layout[i] == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            ss = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
    } else {
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        ss = VK_PIPELINE_STAGE_TRANSFER_BIT;
        ds = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    p_vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
    s_ri_layout[i] = want;
}

static int ri_init(void) {
    if (!pp_init_device_objects() || !pp_init_swapchain_objects()) return 0;
    if (!s_ri_fs) {
        s_ri_fs = make_module(spv_interp_frag, spv_interp_frag_size);
        if (!s_ri_fs) return 0;
    }
    if (!s_ri_dsl) {
        VkDescriptorSetLayoutBinding b[2] = {
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
            { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
        VkDescriptorSetLayoutCreateInfo dci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dci.bindingCount = 2; dci.pBindings = b;
        if (p_vkCreateDescriptorSetLayout(s_dev, &dci, NULL, &s_ri_dsl) != VK_SUCCESS) return 0;
    }
    if (!s_ri_pool) {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_FRAMES * 2 };
        VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = VK_FRAMES; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if (p_vkCreateDescriptorPool(s_dev, &pci, NULL, &s_ri_pool) != VK_SUCCESS) return 0;
        for (int i = 0; i < VK_FRAMES; i++) {
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = s_ri_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &s_ri_dsl;
            if (p_vkAllocateDescriptorSets(s_dev, &ai, &s_ri_ds[i]) != VK_SUCCESS) return 0;
        }
    }
    if (!s_ri_pl) {
        VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RiPush) };
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        li.setLayoutCount = 1; li.pSetLayouts = &s_ri_dsl;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        if (p_vkCreatePipelineLayout(s_dev, &li, NULL, &s_ri_pl) != VK_SUCCESS) return 0;
    }
    if (s_ri_pipe) return 1;
    VkShaderModule vs = s_pp_vs;   /* present.vert: fullscreen triangle */
    if (!vs) return 0;
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s_ri_fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {0};
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2; ci.pStages = st;
    ci.pVertexInputState = &vin; ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp; ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms; ci.pColorBlendState = &cb; ci.pDynamicState = &dy;
    ci.layout = s_ri_pl; ci.renderPass = s_pp_rp;
    if (p_vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &s_ri_pipe) != VK_SUCCESS) {
        s_ri_pipe = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

/* Capture one displayed image into the history ping-pong. Returns 1 when the
 * temporal presenter owns this guest interval (the caller must run the
 * sub-present loop instead of the plain present). */
static int ri_capture(VkImage src, VkImageLayout *src_layout,
                      int dx, int dy, int dw, int dh, int linear,
                      int force43, int path, int ox, int oy) {
    if (!ri_active()) return 0;
    if (!s_ready || !src || dw < 1 || dh < 1) return 0;
    const int S = s_scale;
    int sw = dw * S, sh = dh * S, cw = sw, ch = sh;
    ri_capture_size(sw, sh, force43, &cw, &ch);
    int geometry_changed = (cw != s_ri_w || ch != s_ri_h ||
                            sw != s_ri_src_w || sh != s_ri_src_h ||
                            path != s_ri_path || force43 != s_ri_force43);
    if (geometry_changed) {
        ri_invalidate();
        s_ri_w = cw; s_ri_h = ch;
        s_ri_src_w = sw; s_ri_src_h = sh;
        s_ri_path = path; s_ri_force43 = force43;
    }
    int new_frame = frame_flip_is_new_frame(
        geometry_changed, s_ri_valid == 0, s_perf_cur.draws > 0, ox, oy,
        s_ri_ox, s_ri_oy);
    s_ri_ox = ox; s_ri_oy = oy;
    (void)frame_flip_tracker_vblank(&s_ri_flip, new_frame, &s_ri_lo, &s_ri_hi);
    if (!new_frame) { s_ri_dups++; return 1; }
    if (!ri_ensure(cw, ch)) return 0;

    int dst = 0;
    if (s_ri_valid == 1) dst = s_ri_cur == 0 ? 1 : 0;
    else if (s_ri_valid >= 2) dst = s_ri_prev;

    VkCommandBuffer cb = begin_oneshot();
    /* Source: canonical target or the wide surface, both parked by the caller
     * in their usual layout. Leave the canonical target in TRANSFER_SRC (the
     * plain present does the same); restore the wide surface for its next
     * geometry pass. */
    int is_vram = (src == s_vram_img);
    if (is_vram) vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    else img_to(cb, src, src_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ri_layout_to(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0].x = dx * S;              blit.srcOffsets[0].y = dy * S;
    blit.srcOffsets[1].x = (dx + dw) * S;       blit.srcOffsets[1].y = (dy + dh) * S;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1].x = cw; blit.dstOffsets[1].y = ch; blit.dstOffsets[1].z = 1;
    int downscale = (cw != sw || ch != sh);
    p_vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     s_ri_img[dst], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, (downscale && linear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    ri_layout_to(cb, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!is_vram) img_to(cb, src, src_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    end_oneshot(cb);

    if (s_ri_valid == 0) { s_ri_cur = dst; s_ri_valid = 1; }
    else { s_ri_prev = s_ri_cur; s_ri_cur = dst; s_ri_valid = 2; }
    s_ri_linear = linear;
    s_ri_captures++;
    return 1;
}

/* Sleep to a schedule deadline with the runtime's waitable-timer sleep (the
 * GL presenter uses the same one; SDL_Delay's 1 ms granularity overshot every
 * sub-present). */
extern void psx_host_sleep_ms(unsigned ms);

static void ri_wait_until(uint64_t deadline, uint64_t freq) {
    if (!deadline || !freq) return;
    for (;;) {
        uint64_t now = SDL_GetPerformanceCounter();
        if (now >= deadline) return;
        uint64_t remain = deadline - now;
        uint32_t ms = (uint32_t)((remain * 1000u) / freq);
        if (ms > 1) psx_host_sleep_ms(ms - 1);
    }
}

static int ri_present(float alpha) {
    if (!ri_active() || s_ri_valid < 1) return 0;
    if (!ri_init()) return 0;
    VkImage sc; VkCommandBuffer cb; uint32_t idx, fr;
    latency_ring_mark(LAT_SWAP_BEGIN);
    if (!acquire_present(&sc, &cb, &idx, &fr)) { latency_ring_mark(LAT_SWAP_END); return 0; }
    VkFramebuffer fb = pp_framebuffer(idx);
    if (!fb) { latency_ring_mark(LAT_SWAP_END); return 0; }

    VkImageView pv = s_ri_view[s_ri_prev], cv = s_ri_view[s_ri_cur];
    VkDescriptorImageInfo ii[2] = {
        { s_pp_lin, pv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { s_pp_lin, cv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
    VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                  { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    for (int i = 0; i < 2; i++) {
        w[i].dstSet = s_ri_ds[fr]; w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    p_vkUpdateDescriptorSets(s_dev, 2, w, 0, NULL);

    VkOffset3D dst[2];
    letterbox((int)s_sc_extent.width, (int)s_sc_extent.height,
              s_ri_force43 ? 4 : s_aspect_num, s_ri_force43 ? 3 : s_aspect_den, dst);
    VkClearValue clear = {0};
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = s_pp_rp; bi.framebuffer = fb;
    bi.renderArea.extent = s_sc_extent;
    bi.clearValueCount = 1; bi.pClearValues = &clear;
    p_vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport view = { (float)dst[0].x, (float)dst[0].y,
                        (float)(dst[1].x - dst[0].x), (float)(dst[1].y - dst[0].y),
                        0.0f, 1.0f };
    VkRect2D sci = { { dst[0].x, dst[0].y },
                     { (uint32_t)(dst[1].x - dst[0].x),
                       (uint32_t)(dst[1].y - dst[0].y) } };
    p_vkCmdSetViewport(cb, 0, 1, &view);
    p_vkCmdSetScissor(cb, 0, 1, &sci);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ri_pipe);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ri_pl, 0, 1,
                              &s_ri_ds[fr], 0, NULL);
    RiPush pc = { alpha, s_ri_blend };
    p_vkCmdPushConstants(cb, s_ri_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    perf_draw(cb, 3, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);
    finish_present(cb, sc, idx, fr);
    latency_ring_mark(LAT_SWAP_END);
    s_ri_swaps++;
    pres_record(VK_PRES_INTERP, 0, 0, s_ri_w, s_ri_h,
                dst[0].x, dst[0].y, dst[1].x - dst[0].x, dst[1].y - dst[0].y);
    return 1;
}

/* One guest interval -> N host presents, ending at the interval's deadline so
 * the guest runs at its own cadence (the GL interp_present_source_interval). */
static void ri_present_source_interval(void) {
    uint64_t freq = SDL_GetPerformanceFrequency();
    if (!freq) return;
    uint64_t now = SDL_GetPerformanceCounter();
    if (!frame_interpolation_schedule_begin_phase(
            &s_ri_sched, now, freq, s_ri_source_hz, s_ri_target_hz,
            s_ri_lo, s_ri_hi))
        return;
    uint64_t deadline = 0;
    float alpha = 0.0f;
    while (frame_interpolation_schedule_next(&s_ri_sched, SDL_GetPerformanceCounter(),
                                             &deadline, &alpha)) {
        ri_wait_until(deadline, freq);
        if (!ri_present(alpha)) break;
    }
    ri_wait_until(frame_interpolation_schedule_end(&s_ri_sched), freq);
}

/* ---- public presenter API (mirrors the gl_renderer_* twins) -------------- */
void vk_renderer_set_interpolation(int enabled, double host_hz, double target_hz,
                                   double source_hz, int blend_mode) {
    double effective_hz = target_hz > 0.0 ? target_hz : host_hz;
    if (effective_hz < source_hz) effective_hz = source_hz;
    int active = (enabled && source_hz >= 1.0 && source_hz <= 1000.0 &&
                  effective_hz >= source_hz && effective_hz <= 1000.0) ? 1 : 0;
    if (active != s_ri_enabled || source_hz != s_ri_source_hz ||
        effective_hz != s_ri_target_hz)
        ri_invalidate();
    s_ri_enabled = active;
    s_ri_host_hz = host_hz;
    s_ri_target_hz = active ? effective_hz : 0.0;
    s_ri_source_hz = active ? source_hz : 0.0;
    s_ri_blend = blend_mode == 1 ? 1 : 0;
    s_ri_hold = blend_mode == 2 ? 1 : 0;
    if (active && !s_ri_logged)
        fprintf(stdout, "psxrecomp: vulkan: temporal frame blending enabled: "
                "%.1f presents/s from %.3f guest frames/s (%s; no motion "
                "vectors)\n", effective_hz, source_hz,
                s_ri_hold ? "hold" : s_ri_blend ? "change-adaptive blend"
                                                : "linear blend");
    if (!active && s_ri_logged)
        fprintf(stdout, "psxrecomp: vulkan: temporal frame blending disabled\n");
    s_ri_logged = active;
}

void vk_renderer_set_interpolation_blend(int blend_mode) {
    s_ri_blend = blend_mode == 1 ? 1 : 0;
    s_ri_hold = blend_mode == 2 ? 1 : 0;
}

void vk_renderer_set_interpolation_suspended(int suspended) {
    suspended = suspended ? 1 : 0;
    if (suspended != s_ri_suspended) ri_invalidate();
    s_ri_suspended = suspended;
}

void vk_renderer_set_interpolation_source(int source) {
    source = source == 1 ? 1 : 0;
    if (source != s_ri_source) ri_invalidate();
    s_ri_source = source;
}

int vk_renderer_interpolation_owns_cadence(void) {
    return ri_active();
}

static void ri_release_swapchain(void) {
    if (s_ri_pipe) { p_vkDestroyPipeline(s_dev, s_ri_pipe, NULL); s_ri_pipe = VK_NULL_HANDLE; }
    s_ri_valid = 0;
}

static void ri_destroy_all(void) {
    ri_release_swapchain();
    ri_free_images();
    if (s_ri_fs)   { p_vkDestroyShaderModule(s_dev, s_ri_fs, NULL); s_ri_fs = VK_NULL_HANDLE; }
    if (s_ri_pool) { p_vkDestroyDescriptorPool(s_dev, s_ri_pool, NULL); s_ri_pool = VK_NULL_HANDLE; }
    if (s_ri_pl)   { p_vkDestroyPipelineLayout(s_dev, s_ri_pl, NULL); s_ri_pl = VK_NULL_HANDLE; }
    if (s_ri_dsl)  { p_vkDestroyDescriptorSetLayout(s_dev, s_ri_dsl, NULL); s_ri_dsl = VK_NULL_HANDLE; }
}

int vk_renderer_present_vram(int disp_x, int disp_y, int w, int h,
                             int linear, int force_4_3) {
    if (!s_ctx_ok) return 0;
    flush_cpu_upload();   /* displayed VRAM may include pending CPU writes */
    /* Drain this frame's batch command buffers into ONE submission and let the
     * present CB wait on it in queue order. No vkQueueWaitIdle here: the scene
     * of frame N+1 may overlap present N (the per-present sync was the frame
     * hitch — see docs/RENDERING_ROADMAP.md A.3#2). */
    flush_tex_batch(); flush_geometry();
    /* Temporal 60 fps: snapshot the just-drawn frame into the history
     * ping-pong BEFORE the batch is submitted. When it returns 1 the
     * presenter owns this guest interval and runs the sub-present loop below
     * instead of one swap. */
    int ri_pair = ri_capture(s_vram_img, &s_vram_layout, disp_x, disp_y, w, h,
                             linear, force_4_3, 0, disp_x, disp_y);
    flush_work();
    {   /* PSX_VK_DUMP_TGT diagnostic: periodic full target dump (every 240
         * presents) so the target can be read while the game runs. */
        static int dump_calls;
        const char *dp = getenv("PSX_VK_DUMP_TGT");
        if (dp && *dp && (++dump_calls % 240) == 0) vk_dump_target(dp);
    }
    /* present_shot: serve a staged capture from the VRAM this blit is about to
     * read, before the swap (see shot_serve). It forces its own readback drain
     * when a shot is actually pending. */
    shot_serve(disp_x, disp_y, w, h, NULL,
               force_4_3 ? 4 : s_aspect_num, force_4_3 ? 3 : s_aspect_den);
    if (ri_pair) {
        /* The schedule sleeps to each output deadline and to the end of the
         * guest interval, so the guest keeps its own cadence. */
        ri_present_source_interval();
        perf_snapshot_present();
        return 1;
    }
    VkImage sc; VkCommandBuffer cb; uint32_t idx, fr;
    /* The displayed rect must be inside the window target; grow (once, at a
     * buffer flip into a new area) before the frame is recorded. */
    if (w > 0 && h > 0 &&
        !tgt_require_display(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1)) {
        vk_renderer_present_blank();
        return 1;
    }
    latency_ring_mark(LAT_SWAP_BEGIN);
    if (!acquire_present(&sc, &cb, &idx, &fr)) {   /* frame skipped/recreated */
        latency_ring_mark(LAT_SWAP_END);
        return 1;
    }

    int S = s_scale;
    VkOffset3D dst[2];
    letterbox((int)s_sc_extent.width, (int)s_sc_extent.height,
              force_4_3 ? 4 : s_aspect_num, force_4_3 ? 3 : s_aspect_den, dst);

    /* Opaque shader present first (see pp_draw); blit only as the fallback.
     * uv/texel coords are target-relative: the target holds native
     * [s_tx0,s_ty0]..[+s_tw,+s_th]. */
    float u0 = (disp_x - s_tx0) / (float)s_tw;
    float v0 = (disp_y - s_ty0) / (float)s_th;
    float u1 = (disp_x + w - s_tx0) / (float)s_tw;
    float v1 = (disp_y + h - s_ty0) / (float)s_th;
    vram_to(cb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    /* [video] volumetrics / light shafts take over the present draw; with the
     * effects off this is the unchanged pp_draw call. */
    int drawn = (s_vfx_fog || s_vfx_shafts)
        ? vfx_present_draw(cb, idx, fr, s_vram_view, linear, dst, u0, v0, u1, v1)
        : pp_draw(cb, idx, fr, s_vram_view, linear, dst, u0, v0, u1, v1);
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);   /* re-park */
    if (drawn) {
        finish_present(cb, sc, idx, fr);
        latency_ring_mark(LAT_SWAP_END);
        perf_snapshot_present();
        pres_record(VK_PRES_VRAM, disp_x, disp_y, w, h, dst[0].x, dst[0].y,
                    dst[1].x - dst[0].x, dst[1].y - dst[0].y);
        return 1;
    }

    img_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* clear the swapchain (letterbox bars) to black */
    VkClearColorValue black = {{0,0,0,1}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);

    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0].x = (disp_x - s_tx0) * S;        blit.srcOffsets[0].y = (disp_y - s_ty0) * S;
    blit.srcOffsets[1].x = (disp_x + w - s_tx0) * S;    blit.srcOffsets[1].y = (disp_y + h - s_ty0) * S;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = dst[0]; blit.dstOffsets[1] = dst[1];

    /* VRAM image is parked in TRANSFER_SRC_OPTIMAL. */
    p_vkCmdBlitImage(cb, s_vram_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

    finish_present(cb, sc, idx, fr);
    latency_ring_mark(LAT_SWAP_END);
    perf_snapshot_present();
    pres_record(VK_PRES_VRAM, disp_x, disp_y, w, h, dst[0].x, dst[0].y,
                dst[1].x - dst[0].x, dst[1].y - dst[0].y);
    return 1;
}

void vk_renderer_present_blank(void) {
    if (!s_ctx_ok) return;
    flush_tex_batch(); flush_geometry(); flush_work();
    /* present_shot: display disabled — claim the request so the client's poll
     * terminates, with the black frame that is actually on screen. */
    shot_serve(0, 0, 0, 0, NULL, s_aspect_num, s_aspect_den);
    VkImage sc; VkCommandBuffer cb; uint32_t idx, fr;
    latency_ring_mark(LAT_SWAP_BEGIN);
    if (!acquire_present(&sc, &cb, &idx, &fr)) { latency_ring_mark(LAT_SWAP_END); return; }
    img_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {{0,0,0,1}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
    finish_present(cb, sc, idx, fr);
    latency_ring_mark(LAT_SWAP_END);
    perf_snapshot_present();
    pres_record(VK_PRES_BLANK, 0, 0, 0, 0, 0, 0,
                (int)s_sc_extent.width, (int)s_sc_extent.height);
}

/* Present an ARGB8888 image (24-bit FMV / display-disabled clear) letterboxed.
 * The pixels are 0xAARRGGBB; little-endian byte order [B,G,R,A] matches a
 * B8G8R8A8_UNORM staging image, blitted straight to the swapchain. */
/* CPU-present resource cache (FMV path): the depth24 compose presents a new
 * frame 15-24x/s for minutes; creating + destroying a VkImage and a staging
 * buffer per frame is the same driver-allocation churn class the batched
 * upload fix killed. Cache both keyed by (w,h) — zero allocations at steady
 * state; the staging stays persistently mapped. Freed on resize + shutdown.
 * The per-frame vkQueueWaitIdle below also makes reuse race-free (the copy
 * from the staging has fully completed before the next frame rewrites it). */
static VkImage        s_cpres_img  = VK_NULL_HANDLE;
static VkDeviceMemory s_cpres_imem = VK_NULL_HANDLE;
static VkImageLayout  s_cpres_layout = VK_IMAGE_LAYOUT_UNDEFINED;
static VkBuffer       s_cpres_buf  = VK_NULL_HANDLE;
static VkDeviceMemory s_cpres_mem  = VK_NULL_HANDLE;
static void          *s_cpres_map  = NULL;
static int            s_cpres_w = 0, s_cpres_h = 0;

static void cpres_cache_free(void) {
    if (s_cpres_img)  { p_vkDestroyImage(s_dev, s_cpres_img, NULL);  s_cpres_img = VK_NULL_HANDLE; }
    if (s_cpres_imem) { p_vkFreeMemory(s_dev, s_cpres_imem, NULL);   s_cpres_imem = VK_NULL_HANDLE; }
    if (s_cpres_buf)  {
        staging_destroy(s_cpres_buf, s_cpres_mem);
        s_cpres_buf = VK_NULL_HANDLE;
        s_cpres_mem = VK_NULL_HANDLE;
    }
    s_cpres_map = NULL; s_cpres_w = s_cpres_h = 0;
    s_cpres_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void vk_renderer_present_cpu(const uint32_t *pixels, int src_w, int src_h,
                             int linear, int force_4_3) {
    (void)force_4_3;
    if (!s_ctx_ok || !pixels || src_w <= 0 || src_h <= 0) { vk_renderer_present_blank(); return; }
    flush_tex_batch(); flush_geometry(); flush_work();
    /* present_shot: the composed CPU frame is already here — no VRAM decode. */
    shot_serve(0, 0, src_w, src_h, pixels,
               force_4_3 ? 4 : s_aspect_num, force_4_3 ? 3 : s_aspect_den);

    if (s_cpres_w != src_w || s_cpres_h != src_h) {
        p_vkQueueWaitIdle(s_queue);      /* old resources may be in flight */
        cpres_cache_free();
        if (!make_staging((VkDeviceSize)src_w * src_h * 4,
                          &s_cpres_buf, &s_cpres_mem, &s_cpres_map)) {
            vk_renderer_present_blank(); return;
        }
        if (!make_image(VK_FORMAT_B8G8R8A8_UNORM, src_w, src_h,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &s_cpres_img, &s_cpres_imem, NULL)) {
            cpres_cache_free(); vk_renderer_present_blank(); return;
        }
        s_cpres_w = src_w; s_cpres_h = src_h;
        s_cpres_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    memcpy(s_cpres_map, pixels, (size_t)src_w * src_h * 4);
    VkBuffer buf = s_cpres_buf;
    VkImage img = s_cpres_img;

    VkImage sc; VkCommandBuffer cb; uint32_t idx, fr;
    latency_ring_mark(LAT_SWAP_BEGIN);
    if (!acquire_present(&sc, &cb, &idx, &fr)) { latency_ring_mark(LAT_SWAP_END); return; }
    img_to(cb, img, &s_cpres_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bc = {0};
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = src_w; bc.imageExtent.height = src_h; bc.imageExtent.depth = 1;
    p_vkCmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    img_to(cb, img, &s_cpres_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    img_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {{0,0,0,1}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);

    VkOffset3D dst[2];
    letterbox((int)s_sc_extent.width, (int)s_sc_extent.height, force_4_3 ? 4 : s_aspect_num, force_4_3 ? 3 : s_aspect_den, dst);
    /* Short GP1(07h) bands: letterbox inside the 4:3 rect (see GL present). */
    if (src_h > 0 && src_h < 240) {
        int box_h = dst[1].y - dst[0].y;
        int content_h = (box_h * src_h) / 240;
        if (content_h < 1) content_h = 1;
        int y0 = dst[0].y + (box_h - content_h) / 2;
        dst[0].y = y0;
        dst[1].y = y0 + content_h;
    }
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = src_w; blit.srcOffsets[1].y = src_h; blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = dst[0]; blit.dstOffsets[1] = dst[1];
    p_vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    finish_present(cb, sc, idx, fr);

    /* Cached resources are rewritten next frame: wait for this frame's copy +
     * blit to complete first (FMV cadence is 15-24 fps; one idle-wait per
     * movie frame is cheap, and it is what makes the persistent staging safe). */
    p_vkQueueWaitIdle(s_queue);
    latency_ring_mark(LAT_SWAP_END);
    perf_snapshot_present();
    pres_record(VK_PRES_CPU, 0, 0, src_w, src_h, dst[0].x, dst[0].y,
                dst[1].x - dst[0].x, dst[1].y - dst[0].y);
}

/* GPU-direct native-wide present: blit the displayed buffer's wide surface
 * band straight to the swapchain (no readback), mirroring
 * gl_renderer_present_wide_fbo. Letterbox aspect generalises past 16:9:
 * the 4:3 native frame widened by EXTRA presents at (4*wide_w : 3*native_w).
 * Returns 0 when no wide surface exists for disp_x (caller falls back). */
int vk_renderer_present_wide(int disp_x, int disp_y, int disp_h, int linear) {
    if (!s_ctx_ok || !s_ready || s_wide_w <= 0) return 0;
    int i = -1;
    for (int k = 0; k < VK_WIDE_MAX_SURF; k++)
        if (s_wide_img[k] && s_wide_base[k] == disp_x) { i = k; break; }
    if (i < 0) return 0;
    flush_cpu_upload();
    flush_tex_batch(); flush_geometry();
    /* Temporal 60 fps on the wide surface: snapshot the wide band, then let the
     * presenter own the interval (the wide aspect is already the window's). */
    int ri_pair = ri_capture(s_wide_img[i], &s_wide_layout[i], 0, disp_y,
                             s_wide_w, disp_h, linear, 0, 1, disp_x, disp_y);
    flush_work();   /* present waits in queue order */
    VkImage sc; VkCommandBuffer cb; uint32_t idx, fr;
    latency_ring_mark(LAT_SWAP_BEGIN);
    if (ri_pair) {
        ri_present_source_interval();
        perf_snapshot_present();
        return 1;
    }
    if (!acquire_present(&sc, &cb, &idx, &fr)) { latency_ring_mark(LAT_SWAP_END); return 1; }

    int S = s_scale;
    int native_w = s_wide_w - 2 * s_wide_offset;
    if (native_w <= 0) native_w = s_wide_w;
    /* present_shot: the wide margins exist only on the GPU surface, so capture
     * the canonical display span the VRAM mirror holds (4:3 inside the wide
     * window) — narrow, never black. */
    shot_serve(disp_x, disp_y, native_w, disp_h, NULL, 4, 3);
    VkOffset3D dst[2];
    letterbox((int)s_sc_extent.width, (int)s_sc_extent.height,
              4 * s_wide_w, 3 * native_w, dst);

    /* Opaque shader present first (see pp_draw); blit only as the fallback. */
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    int drawn = (s_vfx_fog || s_vfx_shafts)
        ? vfx_present_draw(cb, idx, fr, s_wide_view[i], linear, dst,
                           0.0f, disp_y / (float)VRAM_H,
                           1.0f, (disp_y + disp_h) / (float)VRAM_H)
        : pp_draw(cb, idx, fr, s_wide_view[i], linear, dst,
                  0.0f, disp_y / (float)VRAM_H,
                  1.0f, (disp_y + disp_h) / (float)VRAM_H);
    if (drawn) {
        img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        finish_present(cb, sc, idx, fr);
        latency_ring_mark(LAT_SWAP_END);
        perf_snapshot_present();
        pres_record(VK_PRES_WIDE, 0, disp_y, s_wide_w, disp_h,
                    dst[0].x, dst[0].y, dst[1].x - dst[0].x, dst[1].y - dst[0].y);
        return 1;
    }

    img_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);

    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    int sy0 = disp_y * S, sy1 = (disp_y + disp_h) * S;
    if (sy0 < 0) sy0 = 0;
    if (sy1 > VRAM_H * S) sy1 = VRAM_H * S;
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0].x = 0;              blit.srcOffsets[0].y = sy0;
    blit.srcOffsets[1].x = s_wide_w * S;   blit.srcOffsets[1].y = sy1;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = dst[0]; blit.dstOffsets[1] = dst[1];
    p_vkCmdBlitImage(cb, s_wide_img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    finish_present(cb, sc, idx, fr);
    latency_ring_mark(LAT_SWAP_END);
    perf_snapshot_present();
    pres_record(VK_PRES_WIDE, 0, disp_y, s_wide_w, disp_h,
                dst[0].x, dst[0].y, dst[1].x - dst[0].x, dst[1].y - dst[0].y);
    return 1;
}

void vk_renderer_sync_cpu(void) {
    if (!s_ctx_ok) return;
    ensure_cpu();   /* drain batches, pack hr->raw, copy raw->CPU mirror */
}

/* Dump the last `count` per-frame perf records as a JSON array (oldest..newest).
 * Always-on; the debug-server "vk_perf" command reads this to find which op
 * explodes during a transition stall (e.g. allocs jumping 10 -> thousands). */
int vk_perf_json(char *out, int cap, int count) {
    uint32_t total = s_perf_head;
    uint32_t n = ((uint32_t)count < total) ? (uint32_t)count : total;
    if (n > VK_PERF_RING) n = VK_PERF_RING;
    int o = snprintf(out, cap, "[");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = total - n + i;          /* frame index, oldest..newest */
        VkPerf *p = &s_perf_ring[k % VK_PERF_RING];
        o += snprintf(out + o, cap - o,
            "%s{\"f\":%u,\"qpc\":%llu,\"wait_us\":%u,\"acquire_us\":%u,\"present_us\":%u,"
            "\"alloc\":%u,\"alloc_kb\":%u,\"oneshot\":%u,\"submit\":%u,"
            "\"sync\":%u,\"pack\":%u,\"blit\":%u,\"upload\":%u,\"copy\":%u,"
            "\"geo\":%u,\"tex\":%u,\"wide\":%u,\"wclr\":%u}",
            i ? "," : "",
            p->present_idx, (unsigned long long)p->present_qpc,
            p->wait_us, p->acquire_us, p->present_us,
            p->allocs, p->alloc_kb, p->oneshots, p->submits,
            p->syncs, p->pack_flushes, p->blits, p->upload_blocks, p->copy_rects,
            p->geo_flushes, p->tex_flushes, p->wide_passes, p->wide_clears);
        if (o >= cap - 256) break;
    }
    o += snprintf(out + o, cap - o, "]");
    return o;
}

/* Sort helper for the percentile aggregates (vk_frame_perf). */
static int perf_cmp_f(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* "vk_frame_perf": per-frame vk_perf-style counters PLUS CPU/GPU timing, in the
 * shape tools/we/perf.py consumes for both backends (the GL backend answers
 * "frame_perf" with the same three figures: total CPU, scene GPU, present GPU).
 * Aggregates are over the last `count` frames; "frames" carries the raw samples
 * so the tool can print p50/p95/max. Never blocks on the GPU: timestamp results
 * are read with VK_QUERY_RESULT_WAIT_BIT clear, a few frames late. */
int vk_frame_perf_json(char *out, int cap, int count) {
    perf_collect_gpu();
    if (count <= 0) count = 240;
    uint32_t total = s_perf_head;
    uint32_t n = (uint32_t)count < total ? (uint32_t)count : total;
    if (n > VK_PERF_RING) n = VK_PERF_RING;

    static float cpu_s[VK_PERF_RING], sc_s[VK_PERF_RING], pr_s[VK_PERF_RING];
    static float vfx_s[VK_PERF_RING];
    uint32_t ncpu = 0, nsc = 0, npr = 0, nvfx = 0;
    double cpu_sum = 0, sc_sum = 0, pr_sum = 0, vfx_sum = 0;
    double al_sum = 0, su_sum = 0, dr_sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPerf *p = &s_perf_ring[(total - n + i) % VK_PERF_RING];
        cpu_s[ncpu++] = p->cpu_ms; cpu_sum += p->cpu_ms;
        al_sum += p->allocs; su_sum += p->submits; dr_sum += p->draws;
        if (p->has_gpu) {
            sc_s[nsc++] = p->scene_gpu_ms; sc_sum += p->scene_gpu_ms;
            pr_s[npr++] = p->present_gpu_ms; pr_sum += p->present_gpu_ms;
        }
        if (p->has_vfx) { vfx_s[nvfx++] = p->vfx_ms; vfx_sum += p->vfx_ms; }
    }
    qsort(cpu_s, ncpu, sizeof(float), perf_cmp_f);
    if (nsc) qsort(sc_s, nsc, sizeof(float), perf_cmp_f);
    if (npr) qsort(pr_s, npr, sizeof(float), perf_cmp_f);
    if (nvfx) qsort(vfx_s, nvfx, sizeof(float), perf_cmp_f);
    float cpu_p50 = ncpu ? cpu_s[(int)(ncpu * 0.50f)] : 0.0f;
    float cpu_p95 = ncpu ? cpu_s[ncpu > 1 ? (int)(ncpu * 0.95f) : 0] : 0.0f;
    float sc_max  = nsc ? sc_s[nsc - 1] : 0.0f;
    float pr_max  = npr ? pr_s[npr - 1] : 0.0f;
    float vfx_max = nvfx ? vfx_s[nvfx - 1] : 0.0f;

    int o = snprintf(out, cap,
        "{\"backend\":\"vulkan\",\"samples\":%u,\"scale\":%d,\"gpu_ts\":%d,"
        "\"target\":[%d,%d,%d,%d],\"target_pct\":%.3f,\"target_grows\":%d,"
        "\"all\":{\"cpu_ms_avg\":%.3f,\"cpu_ms_p50\":%.3f,\"cpu_ms_p95\":%.3f,"
        "\"cpu_ms_max\":%.3f,\"scene_gpu_ms_avg\":%.3f,\"scene_gpu_ms_max\":%.3f,"
        "\"present_gpu_ms_avg\":%.3f,\"present_gpu_ms_max\":%.3f,"
        "\"vfx_ms_avg\":%.3f,\"vfx_ms_max\":%.3f,"
        "\"allocs_avg\":%.1f,\"submits_avg\":%.2f,\"draws_avg\":%.1f},"
        "\"frames\":[",
        n, s_scale, s_ts_ok ? (int)npr : 0,
        s_tx0, s_ty0, s_tw, s_th, (double)s_tgt_pct, s_tgt_grows,
        ncpu ? cpu_sum / ncpu : 0.0, (double)cpu_p50, (double)cpu_p95,
        ncpu ? (double)cpu_s[ncpu - 1] : 0.0,
        nsc ? sc_sum / nsc : 0.0, (double)sc_max,
        npr ? pr_sum / npr : 0.0, (double)pr_max,
        nvfx ? vfx_sum / nvfx : 0.0, (double)vfx_max,
        n ? al_sum / n : 0.0, n ? su_sum / n : 0.0, n ? dr_sum / n : 0.0);
    for (uint32_t i = 0; i < n; i++) {
        VkPerf *p = &s_perf_ring[(total - n + i) % VK_PERF_RING];
        o += snprintf(out + o, cap - o,
            "%s{\"f\":%u,\"cpu_ms\":%.3f,\"scene_gpu_ms\":%.3f,"
            "\"present_gpu_ms\":%.3f,\"vfx_ms\":%.3f,\"gpu\":%u,\"alloc\":%u,\"submit\":%u,"
            "\"draws\":%u,\"geo\":%u,\"tex\":%u,\"blit\":%u,\"sync\":%u,"
            "\"wait_us\":%u,\"tgt\":[%d,%d,%d,%d],\"cover\":[%d,%d,%d,%d],\"oob\":%u}",
            i ? "," : "", p->present_idx, (double)p->cpu_ms,
            (double)p->scene_gpu_ms, (double)p->present_gpu_ms, (double)p->vfx_ms, p->has_gpu,
            p->allocs, p->submits, p->draws, p->geo_flushes, p->tex_flushes,
            p->blits, p->syncs, p->wait_us,
            p->tx0, p->ty0, p->tw, p->th,
            p->cx0, p->cy0, p->cx1, p->cy1, p->oob);
        if (o >= cap - 360) break;
    }
    o += snprintf(out + o, cap - o, "]}");
    return o;
}

/* ---- render-pass + coherency helpers ----------------------------------- */
/* Half an HR pixel in native units, minus 1/64 to keep exact-edge samples off
 * texel centres (see geo.vert / the GL backend's u_shift). */
static float px_shift(void) { return 0.5f / (float)s_scale - 1.0f / 64.0f; }

/* Full-VRAM viewport mapped into a target that starts at (s_tx0,s_ty0): the
 * negative origin keeps every vertex/shader coordinate in native VRAM space
 * while the framebuffer only covers the displayed rect. */
static void tgt_viewport(VkViewport *vp) {
    vp->x = -(float)(s_tx0 * s_scale);
    vp->y = -(float)(s_ty0 * s_scale);
    vp->width = (float)(VRAM_W * s_scale);
    vp->height = (float)(VRAM_H * s_scale);
    vp->minDepth = 0.0f; vp->maxDepth = 1.0f;
}

static void set_vp_full(VkCommandBuffer cb) {
    VkViewport vp; tgt_viewport(&vp);
    p_vkCmdSetViewport(cb, 0, 1, &vp);
}
static void set_scissor_px(VkCommandBuffer cb, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > VRAM_W) w = VRAM_W - x; if (y + h > VRAM_H) h = VRAM_H - y;
    if (w < 0) w = 0; if (h < 0) h = 0;
    /* framebuffer coords = native - target origin, clipped to the target */
    int fx = x - s_tx0, fy = y - s_ty0;
    int fx1 = fx + w, fy1 = fy + h;
    if (fx < 0) fx = 0; if (fy < 0) fy = 0;
    if (fx1 > s_tw) fx1 = s_tw; if (fy1 > s_th) fy1 = s_th;
    if (fx1 < fx) fx1 = fx; if (fy1 < fy) fy1 = fy;
    VkRect2D sc = { { fx * s_scale, fy * s_scale },
                    { (uint32_t)((fx1 - fx) * s_scale), (uint32_t)((fy1 - fy) * s_scale) } };
    p_vkCmdSetScissor(cb, 0, 1, &sc);
}

/* A same-layout transition is elided by img_to(), while the render pass has
 * no external dependency. Explicitly order a preceding pass's attachment
 * writes before a following LOAD/blend pass on the same color image. */
static void color_self_barrier(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);
}

static void begin_geo_pass(VkCommandBuffer cb) {
    /* Explicit stencil ordering across one-shot submits: this pass both TESTS
     * and WRITES the stencil (PSX mask bits) that a PRIOR submit's pass wrote.
     * Cross-submit ordering previously rode on the COLOR image barriers'
     * execution dependencies alone — no memory dependency covered the DS
     * attachment, which is spec-fragile (worked on this NVIDIA driver, could
     * produce mask-bit flicker elsewhere). A self-barrier on the stencil
     * aspect (late-tests write -> early-tests read|write) makes prior stencil
     * writes visible before this pass's tests, per submission order. */
    VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    db.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    db.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = s_ds_img;
    db.subresourceRange.aspectMask = DS_ASPECT;
    db.subresourceRange.levelCount = 1;
    db.subresourceRange.layerCount = 1;
    db.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, NULL, 0, NULL, 1, &db);
    color_self_barrier(cb, s_vram_img);
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = s_rpass; rp.framebuffer = s_fbo;
    rp.renderArea.extent.width = (uint32_t)(s_tw * s_scale);
    rp.renderArea.extent.height = (uint32_t)(s_th * s_scale);
    if (s_vfx_fog && s_rpass_fog) {
        /* [video] volumetrics: clear the depth aspect to 0.0 (the fog factor
         * "no fade") on the frame's FIRST geometry pass, then LOAD it for the
         * rest so a later batch's proxy does not wipe the earlier ones. The
         * stencil (mask bits) always loads, as in s_rpass. */
        VkClearValue cv[2];
        cv[0].color.float32[0] = 0.0f; cv[0].color.float32[1] = 0.0f;
        cv[0].color.float32[2] = 0.0f; cv[0].color.float32[3] = 0.0f;
        cv[1].depthStencil.depth = 0.0f; cv[1].depthStencil.stencil = 0;
        rp.renderPass = s_vfx_depth_cleared ? s_rpass_fog_load : s_rpass_fog;
        rp.clearValueCount = 2; rp.pClearValues = cv;
        s_vfx_depth_cleared = 1;
    }
    p_vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
}
/* Bind a graphics pipeline implementing GL's mask_stencil: check==0 -> SET
 * (always+replace, ref=write_val); check!=0 -> test stored==0 and write 1 via
 * INVERT (write_val!=0) or leave (write_val==0), ref=0. */
static void bind_masked(VkCommandBuffer cb, int prog, int topo, int blend, int check, int write_val) {
    int stencil = !check ? 0 : (write_val ? 2 : 1);
    VkPipeline p = get_pipeline(prog, topo, blend, stencil, 0);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    p_vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, check ? 0u : (uint32_t)write_val);
}
/* Like bind_masked but with ALL colour writes disabled (stencil-only pass). */
static void bind_masked_stencil_only(VkCommandBuffer cb, int prog, int topo, int check, int write_val) {
    int stencil = !check ? 0 : (write_val ? 2 : 1);
    VkPipeline p = get_pipeline(prog, topo, 0, stencil, 1);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    p_vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, check ? 0u : (uint32_t)write_val);
}

/* Host-visible staging buffer (TRANSFER_SRC). */
static int make_staging(VkDeviceSize bytes, VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    int best = -1;
    for (int i = 0; i < STAGING_CACHE_MAX; ++i) {
        StagingCacheEntry *entry = &s_staging_cache[i];
        if (!entry->busy && entry->buf && entry->size >= bytes &&
            (best < 0 || entry->size < s_staging_cache[best].size))
            best = i;
    }
    if (best >= 0) {
        StagingCacheEntry *entry = &s_staging_cache[best];
        entry->busy = 1;
        *buf = entry->buf;
        *mem = entry->mem;
        *map = entry->map;
        return 1;
    }

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    /* Staging buffers serve uploads (copy source) AND readbacks via
     * vkCmdCopyImageToBuffer (copy destination). */
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (p_vkCreateBuffer(s_dev, &bci, NULL, buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req; p_vkGetBufferMemoryRequirements(s_dev, *buf, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = find_mem_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mi.memoryTypeIndex == UINT32_MAX || p_vkAllocateMemory(s_dev, &mi, NULL, mem) != VK_SUCCESS) {
        p_vkDestroyBuffer(s_dev, *buf, NULL); return 0;
    }
    p_vkBindBufferMemory(s_dev, *buf, *mem, 0);
    if (p_vkMapMemory(s_dev, *mem, 0, bytes, 0, map) != VK_SUCCESS) {
        p_vkDestroyBuffer(s_dev, *buf, NULL);
        p_vkFreeMemory(s_dev, *mem, NULL);
        return 0;
    }
    for (int i = 0; i < STAGING_CACHE_MAX; ++i) {
        if (!s_staging_cache[i].buf) {
            s_staging_cache[i].buf = *buf;
            s_staging_cache[i].mem = *mem;
            s_staging_cache[i].size = bytes;
            s_staging_cache[i].map = *map;
            s_staging_cache[i].busy = 1;
            break;
        }
    }
    s_perf_cur.allocs++;
    s_perf_cur.alloc_kb += (uint32_t)(bytes / 1024);
    return 1;
}
static void free_staging(VkBuffer buf, VkDeviceMemory mem) {
    staging_release(buf, mem);
}

/* ---- HD override images ------------------------------------------------ */
/* Point one cached descriptor set at (raw VRAM mirror, override image). The
 * layout is s_dsl_tex, so the same pipeline layout/pipeline serves both the
 * native and the overridden draw. */
static void hd_write_set(VkDescriptorSet set, VkImageView hd_view) {
    VkDescriptorImageInfo raw_smp = { s_samp, s_raw_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo hd_smp  = { s_samp, hd_view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w[2] = {0};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = set; w[0].dstBinding = 0;
    w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &raw_smp;
    w[1] = w[0]; w[1].dstBinding = 1; w[1].pImageInfo = &hd_smp;
    p_vkUpdateDescriptorSets(s_dev, 2, w, 0, NULL);
}

static void hd_texture_destroy(HdVkTexture *t) {
    if (t->view) p_vkDestroyImageView(s_dev, t->view, NULL);
    if (t->img)  p_vkDestroyImage(s_dev, t->img, NULL);
    if (t->mem)  p_vkFreeMemory(s_dev, t->mem, NULL);
    s_hd_cache_bytes -= t->bytes;
    VkDescriptorSet set = t->set;
    memset(t, 0, sizeof(*t));
    t->set = set;   /* the set outlives the image and is re-pointed on reuse */
}

/* Upload one decoded override (RGBA8) and return its descriptor set. `mode` is
 * the alpha class from the pack and `src` the (origin_u, origin_v, source_w,
 * source_h) rectangle the image covers in native texture space. */
static VkDescriptorSet hd_texture(const GpuHdTextureImage *image, int *mode,
                                  float src[4]) {
    if (!image || !image->rgba || !image->width || !image->height ||
        !image->source_width || !image->source_height ||
        image->stride != (size_t)image->width * 4u ||
        (int)image->width > s_max_image_dim || (int)image->height > s_max_image_dim)
        return VK_NULL_HANDLE;
    const size_t bytes = (size_t)image->stride * image->height;
    if (bytes > VK_HD_CACHE_BUDGET) return VK_NULL_HANDLE;

    HdVkTexture *slot = NULL, *victim = NULL;
    for (int i = 0; i < VK_HD_CACHE_MAX; ++i) {
        HdVkTexture *t = &s_hd_cache[i];
        if (t->img && t->key == image->cache_key && t->generation == image->generation) {
            t->last_use = ++s_hd_cache_clock;
            *mode = image->alpha_mode;
            src[0] = (float)image->origin_u; src[1] = (float)image->origin_v;
            src[2] = (float)image->source_width; src[3] = (float)image->source_height;
            return t->set;
        }
        if (!t->img && !slot) slot = t;
        if (t->img && (!victim || t->last_use < victim->last_use)) victim = t;
    }
    if (!slot) {
        if (!victim) return VK_NULL_HANDLE;
        /* Freeing an image whose descriptor set may still be referenced by a
         * submitted draw needs a full drain; evictions are rare (64 entries). */
        flush_tex_batch();
        gpu_sync();
        hd_texture_destroy(victim);
        slot = victim;
    }
    if (s_hd_cache_bytes + bytes > VK_HD_CACHE_BUDGET && victim && victim != slot) {
        flush_tex_batch();
        gpu_sync();
        hd_texture_destroy(victim);
    }
    if (!slot->set) return VK_NULL_HANDLE;

    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, (int)image->width, (int)image->height,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &slot->img, &slot->mem, &slot->view))
        return VK_NULL_HANDLE;
    slot->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkBuffer buf; VkDeviceMemory mem; void *map;
    if (!make_staging((VkDeviceSize)bytes, &buf, &mem, &map)) {
        hd_texture_destroy(slot);
        return VK_NULL_HANDLE;
    }
    memcpy(map, image->rgba, bytes);
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, slot->img, &slot->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy rc = {0};
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rc.imageSubresource.layerCount = 1;
    rc.imageExtent.width = image->width; rc.imageExtent.height = image->height;
    rc.imageExtent.depth = 1;
    p_vkCmdCopyBufferToImage(cb, buf, slot->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    img_to(cb, slot->img, &slot->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_oneshot(cb);
    defer_staging(buf, mem);   /* copy in flight; freed at the next gpu_sync */

    slot->key = image->cache_key; slot->generation = image->generation;
    slot->w = image->width; slot->h = image->height; slot->bytes = bytes;
    slot->last_use = ++s_hd_cache_clock;
    s_hd_cache_bytes += bytes;
    hd_write_set(slot->set, slot->view);
    gpu_hd_textures_note_texture(image->width, image->height);

    *mode = image->alpha_mode;
    src[0] = (float)image->origin_u; src[1] = (float)image->origin_v;
    src[2] = (float)image->source_width; src[3] = (float)image->source_height;
    return slot->set;
}

/* Drop every override image (pack reload/shutdown, savestate restage). */
static void hd_cache_clear(void) {
    if (!s_dev) return;
    for (int i = 0; i < VK_HD_CACHE_MAX; ++i) {
        if (s_hd_cache[i].img) hd_texture_destroy(&s_hd_cache[i]);
        /* The sets outlive the images: point them back at the dummy so a
         * stale binding can never reference a destroyed view. */
        if (s_hd_cache[i].set && s_hd_dummy_view)
            hd_write_set(s_hd_cache[i].set, s_hd_dummy_view);
    }
    s_hd_cache_bytes = 0;
    s_tb_hd_set = VK_NULL_HANDLE;
}

/* Pack changed (opened, reloaded, closed): drop the materialized overrides and
 * apply the CLUT-row overrides that are already resident in VRAM. Palettes are
 * uploaded at boot, long before a pack is selected, so the upload hook alone
 * would never see them. */
void vk_renderer_hd_pack_changed(void) {
    if (!s_dev) return;
    flush_tex_batch();
    gpu_sync();
    hd_cache_clear();
    if (!s_ready || !s_vram || !vk_hd_pack_active()) return;

    enum { SWEEP_MAX = 512 };
    static VkBufferImageCopy copies[SWEEP_MAX];
    static uint16_t blocks[SWEEP_MAX][16];
    int n = 0;
    for (int y = 0; y < VRAM_H && n < SWEEP_MAX; y++) {
        for (int x = 0; x < VRAM_W; x += 16) {
            const uint16_t *src = s_vram + (size_t)y * VRAM_W + x;
            uint16_t replacement[16];
            if (!vk_hd_pack_clut_lookup(src, replacement)) continue;
            for (int i = 0; i < 16; i++)
                blocks[n][i] = (uint16_t)((src[i] & 0x8000u) | (replacement[i] & 0x7FFFu));
            memset(&copies[n], 0, sizeof(copies[n]));
            copies[n].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copies[n].imageSubresource.layerCount = 1;
            copies[n].imageOffset.x = x; copies[n].imageOffset.y = y;
            copies[n].imageExtent.width = 16; copies[n].imageExtent.height = 1;
            copies[n].imageExtent.depth = 1;
            copies[n].bufferOffset = (VkDeviceSize)n * 32;
            if (++n >= SWEEP_MAX) break;
        }
    }
    if (!n) return;
    VkBuffer buf; VkDeviceMemory mem; void *map;
    if (!make_staging((VkDeviceSize)n * 32, &buf, &mem, &map)) return;
    memcpy(map, blocks, (size_t)n * 32);
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, buf, s_raw_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             (uint32_t)n, copies);
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_oneshot(cb);
    defer_staging(buf, mem);
    gpu_sync();
}

/* hr -> raw mirror: pack the dirty rect via the compute pass (top-left sample of
 * each SxS block). Caller has already drained the batches that dirtied it. */
static void pack_flush(void) {
    if (!s_ready || !s_pack_dirty.set || !s_tgt_ready) return;
    int x = s_pack_dirty.x0, y = s_pack_dirty.y0;
    int x1 = s_pack_dirty.x1, y1 = s_pack_dirty.y1;
    rect_clear(&s_pack_dirty);
    /* Only pixels the target actually holds may be packed; the rest of the raw
     * mirror keeps its exact uploaded content. */
    if (!tgt_clip(&x, &y, &x1, &y1)) return;
    s_perf_cur.pack_flushes++;
    int w = x1 - x + 1;
    int h = y1 - y + 1;
    /* [video] vram_extension: redirect the wide framebuffer's overflow columns
     * into the raw mirror's extension columns (see vram_ext.h). The gate comes
     * from gpu.c (draw area X span == display X span, rows = every learned
     * framebuffer band); a render-to-texture pass leaves it off. The counter
     * splits this rect into present-side redirects and - the assertion -
     * framebuffer columns that would land in guest VRAM. */
    int ex0 = -1, ex1 = 0, ey0 = 0, ey1 = 0;
    int gx0, gy0, gx1, gy1;
    if (vram_ext_region(&gx0, &gy0, &gx1, &gy1)) {
        ex0 = VRAM_EXT_X0; ex1 = gx1; ey0 = gy0; ey1 = gy1;
        /* The band rows are the framebuffer: redirect the whole overflow span
         * even when this dirty rect is narrower (a tile-by-tile frame, or a
         * pack triggered by a texture sample before the right of the frame was
         * drawn). Only inside the band: outside it those columns are texture
         * pages and must stay guest. Re-clipped to the window target. */
        if (y >= gy0 && y1 <= gy1 && gx1 > x1) {
            int nx = x, ny = y, nx1 = gx1, ny1 = y1;
            if (tgt_clip(&nx, &ny, &nx1, &ny1)) {
                x = nx; y = ny; x1 = nx1; y1 = ny1;
                w = x1 - x + 1;
                h = y1 - y + 1;
            }
        }
    }
    vram_ext_note_pack_rect(x, y, x1, y1);
    vram_ext_note_pack_passes(1, (uint64_t)w * (uint64_t)h);
    VkCommandBuffer cb = begin_oneshot();
    vram_to(cb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);     /* hr sampled */
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_GENERAL); /* raw storage */
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe_pack);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_pl_pack, 0, 1, &s_ds_pack, 0, NULL);
    PackPush pp = { s_scale, x, y, s_tx0, s_ty0,
                    ex0, ex1, ey0, ey1, VRAM_EXT_OFFSET };
    p_vkCmdPushConstants(cb, s_pl_pack, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pp, &pp);
    p_vkCmdDispatch(cb, (uint32_t)((w + 7) / 8), (uint32_t)((h + 7) / 8), 1);
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
}

/* Ensure the raw mirror is current for a textured draw sampling the given page
 * / CLUT (mirrors the GL backend). */
static void flush_pack_if_sampling(int tpx, int tpy, int depth, int clx, int cly) {
    if (!s_pack_dirty.set) return;
    int page_w = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    if (rect_intersects(&s_pack_dirty, tpx, tpy, tpx + page_w - 1, tpy + 255)) {
        flush_tex_batch(); pack_flush(); return;
    }
    if (depth <= 1) {
        int n = depth == 0 ? 16 : 256;
        if (rect_intersects(&s_pack_dirty, clx, cly, clx + n - 1, cly)) {
            flush_tex_batch(); pack_flush();
        }
    }
}

/* Blit a native [x,y,w,h] rect from src_img into the hr image (two STP passes,
 * bit15 -> stencil mirror). plain=1 (CPU upload: SET stencil, src already final)
 * or plain=0 (VRAM copy: honour mask set/check). src_div: 1 hr-res src, S native.
 * Marks the dest pack-dirty when mark_pack (copies re-derive raw from hr). */
static void blit_region(int x, int y, int w, int h, VkImageView src_view,
                        VkImage src_img, VkImageLayout *src_layout,
                        int src_div, int sox, int soy, int plain, int mark_pack) {
    if (w <= 0 || h <= 0) return;
    int x1 = x + w - 1, y1 = y + h - 1;
    perf_cover(x, y, x1, y1);
    if (!s_tgt_ready) return;                      /* target not created yet */
    tgt_note_oob(x, y, x1, y1);
    /* Only the part inside the window target can be rendered; the raw mirror
     * is written separately and stays exact, so clipping here only skips
     * pixels that cannot be displayed. */
    if (!tgt_clip(&x, &y, &x1, &y1)) return;
    w = x1 - x + 1; h = y1 - y + 1;
    s_perf_cur.blits++;
    if (s_ds_blit_idx >= BLIT_DESC_RING) gpu_sync();   /* ring exhausted: drain, reset idx */
    VkDescriptorSet ds = s_ds_blit_ring[s_ds_blit_idx++];
    VkDescriptorImageInfo ii = { s_samp, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ds; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    p_vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);

    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, src_img, src_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vram_to(cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_geo_pass(cb);
    set_vp_full(cb); set_scissor_px(cb, x, y, w, h);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl_blit, 0, 1, &ds, 0, NULL);
    BlitPush bp = {0};
    bp.shift = px_shift(); bp.maskset = plain ? 0 : s_mask_set; bp.src_div = src_div;
    /* The fragment's gl_FragCoord is target-local; add the origin back into the
     * source offset so the fetch stays in native VRAM space. */
    bp.src_off[0] = sox + s_tx0 * s_scale / src_div;
    bp.src_off[1] = soy + s_ty0 * s_scale / src_div;
    bp.rect[0] = x; bp.rect[1] = y; bp.rect[2] = x + w; bp.rect[3] = y + h;
    int chk = plain ? 0 : s_mask_check;
    /* pass 1: bit15=0 texels, write stencil 0 (or honour mask set) */
    bind_masked(cb, 2, 0, 0, chk, plain ? 0 : s_mask_set);
    bp.stp_pass = 1;
    p_vkCmdPushConstants(cb, s_pl_blit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
    perf_draw(cb, 6, 1, 0, 0);
    /* pass 2: bit15=1 texels, write stencil 1 */
    bind_masked(cb, 2, 0, 0, chk, 1);
    bp.stp_pass = 2;
    p_vkCmdPushConstants(cb, s_pl_blit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
    perf_draw(cb, 6, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
    if (mark_pack) rect_add(&s_pack_dirty, x, y, x + w - 1, y + h - 1);
    s_gpu_dirty = 1;
}

/* ---- VRAM transfers (CPU <-> GPU image) -------------------------------- */
/* Upload a w x h block of 1555 pixels at (x,y). The raw mirror takes the exact
 * 1555 bits directly (CLUT indices must survive), and the hr render target is
 * updated via a blit so the stencil bit15 mirror stays exact. */
static void vram_upload_block(int x, int y, int w, int h, const uint16_t *data) {
    if (w <= 0 || h <= 0 || !s_ready) return;
    s_perf_cur.upload_blocks++;
    /* Uploads carry textures/CLUTs: they are canonical, never redirected. */
    vram_ext_note_texture_read(0);
    flush_tex_batch(); flush_geometry();

    /* 1) raw mirror: native 1555, exact. */
    VkBuffer rbuf; VkDeviceMemory rmem; void *rmap;
    if (make_staging((VkDeviceSize)w * h * 2, &rbuf, &rmem, &rmap)) {
        memcpy(rmap, data, (size_t)w * h * 2);
        /* CLUT-row overrides land here, at the upload boundary: the GPU mirror
         * gets the re-authored 16-entry palette, the CPU array keeps the disc
         * values (see docs/HD_TEXTURES.md). */
        vk_hd_pack_patch_clut((uint16_t*)rmap, x, y, w, h);
        VkCommandBuffer cb = begin_oneshot();
        img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy rc = {0};
        rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rc.imageSubresource.layerCount = 1;
        rc.imageOffset.x = x; rc.imageOffset.y = y;
        rc.imageExtent.width = w; rc.imageExtent.height = h; rc.imageExtent.depth = 1;
        p_vkCmdCopyBufferToImage(cb, rbuf, s_raw_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
        img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        end_oneshot(cb);
        defer_staging(rbuf, rmem);   /* copy in flight; freed at next gpu_sync */
    }

    /* 2) native RGBA8 staging -> upload image. */
    VkBuffer ubuf; VkDeviceMemory umem; void *umap;
    if (make_staging((VkDeviceSize)w * h * 4, &ubuf, &umem, &umap)) {
        uint8_t *m = (uint8_t*)umap;
        for (int i = 0; i < w * h; i++) rgb555_to_rgba8(data[i], m + (size_t)i * 4);
        VkCommandBuffer cb = begin_oneshot();
        img_to(cb, s_up_img, &s_up_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy uc = {0};
        uc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; uc.imageSubresource.layerCount = 1;
        uc.imageOffset.x = x; uc.imageOffset.y = y;
        uc.imageExtent.width = w; uc.imageExtent.height = h; uc.imageExtent.depth = 1;
        p_vkCmdCopyBufferToImage(cb, ubuf, s_up_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uc);
        end_oneshot(cb);
        defer_staging(ubuf, umem);   /* copy in flight; freed at next gpu_sync */
        /* 3) blit upload image -> hr (native src: src_div=S, off 0). */
        blit_region(x, y, w, h, s_up_view, s_up_img, &s_up_layout, s_scale, 0, 0, 1, 0);
    }
}

/* CPU -> GPU: flush accumulated CPU-side VRAM writes as ONE upload (see
 * s_up_rects). vram_upload_block drains the draw batches first, so PS1
 * submission order is preserved: every draw appended before these writes has
 * landed by the time the upload writes VRAM, and every GPU-op entry point
 * calls this before touching VRAM so the writes land before later ops. */
static void flush_cpu_upload(void) {
    if (s_up_nrects == 0 || !s_ready || !s_vram) return;
    /* Snapshot + clear first (re-entrancy safe: up_add overflow calls back). */
    DirtyRect rects[UP_RECTS_MAX];
    int nrects = s_up_nrects;
    memcpy(rects, s_up_rects, (size_t)nrects * sizeof(DirtyRect));
    s_up_nrects = 0;

    flush_tex_batch(); flush_geometry();   /* PS1 order: queued draws land first */
    s_perf_cur.upload_blocks++;

    /* COALESCED upload: exactly TWO queue submits per flush regardless of rect
     * count (one for the buffer->image copies, one render pass for the hr
     * blits). The naive per-rect vram_upload_block/blit_region loop paid
     * 2 staging allocs + 3 submits PER RECT — with the exact-rect list feeding
     * MDEC macroblock streams that re-created the boot-wedge submit churn the
     * batched-upload fix existed to kill (measured 0.7 fps at the Tomba2
     * Whoopee logo). Rect data is packed back-to-back in ONE 1555 staging +
     * ONE RGBA8 staging, addressed via VkBufferImageCopy bufferOffset. */
    size_t total = 0;
    for (int i = 0; i < nrects; i++)
        total += vk_upload_aligned_texels(
            (size_t)(rects[i].x1 - rects[i].x0 + 1) *
            (size_t)(rects[i].y1 - rects[i].y0 + 1));
    if (total == 0) return;

    VkBuffer rbuf; VkDeviceMemory rmem; void *rmap;
    if (!make_staging((VkDeviceSize)total * 2, &rbuf, &rmem, &rmap)) return;
    VkBuffer ubuf; VkDeviceMemory umem; void *umap;
    if (!make_staging((VkDeviceSize)total * 4, &ubuf, &umem, &umap)) {
        defer_staging(rbuf, rmem);
        return;
    }

    VkBufferImageCopy rcopies[UP_RECTS_MAX], ucopies[UP_RECTS_MAX];
    memset(rcopies, 0, sizeof rcopies); memset(ucopies, 0, sizeof ucopies);
    size_t texoff = 0;
    uint16_t *rdst = (uint16_t *)rmap;
    uint8_t  *udst = (uint8_t *)umap;
    for (int i = 0; i < nrects; i++) {
        int x = rects[i].x0, y = rects[i].y0;
        int w = rects[i].x1 - rects[i].x0 + 1;
        int h = rects[i].y1 - rects[i].y0 + 1;
        for (int row = 0; row < h; row++) {
            const uint16_t *src = s_vram + (size_t)(y + row) * VRAM_W + x;
            memcpy(rdst + texoff + (size_t)row * w, src, (size_t)w * 2);
            uint8_t *m = udst + (texoff + (size_t)row * w) * 4;
            for (int col = 0; col < w; col++)
                rgb555_to_rgba8(src[col], m + (size_t)col * 4);
        }
        rcopies[i].bufferOffset = (VkDeviceSize)texoff * 2;
        /* CLUT-row overrides, upload boundary (raw mirror only). */
        vk_hd_pack_patch_clut(rdst + texoff, x, y, w, h);
        rcopies[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rcopies[i].imageSubresource.layerCount = 1;
        rcopies[i].imageOffset.x = x; rcopies[i].imageOffset.y = y;
        rcopies[i].imageExtent.width = w; rcopies[i].imageExtent.height = h;
        rcopies[i].imageExtent.depth = 1;
        ucopies[i] = rcopies[i];
        ucopies[i].bufferOffset = (VkDeviceSize)texoff * 4;
        /* Padding prevents an odd-sized R16 rect from leaving all following
         * VkBufferImageCopy offsets at two modulo four bytes. */
        texoff += vk_upload_aligned_texels((size_t)w * h);
    }

    /* Submit 1: both image copy sets. */
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, rbuf, s_raw_img,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             (uint32_t)nrects, rcopies);
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    img_to(cb, s_up_img, &s_up_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, ubuf, s_up_img,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             (uint32_t)nrects, ucopies);
    end_oneshot(cb);
    defer_staging(rbuf, rmem);
    defer_staging(ubuf, umem);

    /* Submit 2: one render pass, N scissored 2-draw blits upload-image -> hr
     * (plain blit semantics, matching vram_upload_block's blit_region call:
     * mask writes off, no pack mark — uploaded content IS the CPU data).
     * Clipped to the window target: texture data outside the displayed area
     * only exists in the raw mirror, which submit 1 already wrote exactly. */
    if (!s_tgt_ready) { s_gpu_dirty = 1; return; }
    if (s_ds_blit_idx >= BLIT_DESC_RING) gpu_sync();
    VkDescriptorSet ds = s_ds_blit_ring[s_ds_blit_idx++];
    VkDescriptorImageInfo ii = { s_samp, s_up_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ds; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    p_vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);
    cb = begin_oneshot();
    img_to(cb, s_up_img, &s_up_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vram_to(cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_geo_pass(cb);
    set_vp_full(cb);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl_blit, 0, 1, &ds, 0, NULL);
    for (int i = 0; i < nrects; i++) {
        int x = rects[i].x0, y = rects[i].y0;
        int x1 = rects[i].x1, y1 = rects[i].y1;
        if (!tgt_clip(&x, &y, &x1, &y1)) continue;
        int w = x1 - x + 1;
        int h = y1 - y + 1;
        set_scissor_px(cb, x, y, w, h);
        s_perf_cur.blits++;
        BlitPush bp = {0};
        bp.shift = px_shift(); bp.maskset = 0; bp.src_div = s_scale;
        bp.src_off[0] = s_tx0; bp.src_off[1] = s_ty0;   /* target-local fragcoord */
        bp.rect[0] = x; bp.rect[1] = y; bp.rect[2] = x + w; bp.rect[3] = y + h;
        bind_masked(cb, 2, 0, 0, 0, 0);
        bp.stp_pass = 1;
        p_vkCmdPushConstants(cb, s_pl_blit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
        perf_draw(cb, 6, 1, 0, 0);
        bind_masked(cb, 2, 0, 0, 0, 1);
        bp.stp_pass = 2;
        p_vkCmdPushConstants(cb, s_pl_blit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
        perf_draw(cb, 6, 1, 0, 0);
    }
    p_vkCmdEndRenderPass(cb);
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
    s_gpu_dirty = 1;
}

/* GPU -> CPU mirror: drain batches, pack, copy the whole raw mirror down. */
static void ensure_cpu(void) {
    extern int psx_netplay_active(void);
    if (!s_ready || !s_gpu_dirty || !s_vram) return;
    if (psx_netplay_active()) {
        s_gpu_dirty = 0;
        return;
    }
    flush_cpu_upload();   /* readback overwrites s_vram: pending writes land first */
    flush_tex_batch(); flush_geometry();
    /* Readback must reflect ALL current hr content, not just the incremental
     * s_pack_dirty rect. pack_flush() packs only that rect and early-returns if
     * it is unset — so draws that updated hr (and present) but never marked the
     * pack rect (e.g. the BIOS shell after the logo) left the raw mirror stale,
     * and screenshots/guest VRAM reads returned an old frame (the logo) while
     * the live window showed the shell. Force a full-frame pack here; this path
     * is readback-only (NOT the per-frame present path), so the cost is paid
     * only on screenshots / guest VRAM reads, never every frame. */
    rect_add(&s_pack_dirty, 0, 0, VRAM_W - 1, VRAM_H - 1);
    pack_flush();
    /* Guest plane (0..1023) + the [video] vram_extension plane (1024..1215)
     * come down in one staging buffer, each tightly packed. */
    const size_t guest_bytes = (size_t)VRAM_W * VRAM_H * 2;
    const size_t ext_bytes   = (size_t)VRAM_EXT_COLS * VRAM_H * 2;
    int ext_live = vram_ext_driving();
    size_t total_bytes = guest_bytes + (ext_live ? ext_bytes : 0);
    VkBuffer buf; VkDeviceMemory mem; void *map;
    if (!make_staging((VkDeviceSize)total_bytes, &buf, &mem, &map)) return;
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rc = {0};
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rc.imageSubresource.layerCount = 1;
    rc.bufferRowLength = VRAM_W;
    rc.imageExtent.width = VRAM_W; rc.imageExtent.height = VRAM_H; rc.imageExtent.depth = 1;
    p_vkCmdCopyImageToBuffer(cb, s_raw_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rc);
    if (ext_live) {
        rc.bufferOffset = (VkDeviceSize)guest_bytes;
        rc.bufferRowLength = VRAM_EXT_COLS;
        /* The raw mirror is VRAM_EXT_HOST_W wide: guest columns 0..1023, then
         * the extension plane at host column VRAM_EXT_HOST_X0 = 1024. Reading
         * at VRAM_EXT_OFFSET (512) loaded guest texture pages into the plane —
         * the frame's right columns showed textures (the GL path fixed the
         * same read). */
        rc.imageOffset.x = VRAM_EXT_HOST_X0; rc.imageOffset.y = 0;
        rc.imageExtent.width = VRAM_EXT_COLS;
        p_vkCmdCopyImageToBuffer(cb, s_raw_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rc);
    }
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_oneshot(cb);
    gpu_sync();   /* the readback result must be complete before we read it */
    memcpy(s_vram, map, guest_bytes);
    if (ext_live) {
        memcpy(vram_ext_host(), (const uint8_t *)map + guest_bytes, ext_bytes);
        vram_ext_note_readback((uint64_t)ext_bytes);
    }
    free_staging(buf, mem);
    s_gpu_dirty = 0;
}

/* ---- backend vtable ---------------------------------------------------- */
/* Also initialize the software renderer on the SAME VRAM array so a failed
 * Vulkan context creation can fall back to software cleanly (mirrors how the GL
 * backend's init calls sw_renderer_init). When Vulkan is active the SW mirror
 * is just the CPU readback target. */
static void vkb_init(uint16_t *vram) { s_vram = vram; s_cpu_dirty = 0; sw_renderer_init(vram); }
/* vkb_scale value: gr_set_scale can arrive before the context exists (init) or,
 * for dynamic resolution, later. A later change re-creates the windowed target
 * at the new scale; its content is re-rendered by the game (VRAM itself, and
 * the raw mirror, are unaffected). */
static void vkb_set_scale(int scale) {
    if (scale < 1) return;
    if (scale > VK_MAX_INTERNAL_SCALE) scale = VK_MAX_INTERNAL_SCALE;
    if (scale == s_scale) return;
    s_scale = scale;
    if (!s_ready) return;
    flush_tex_batch(); flush_geometry(); gpu_sync();
    ri_invalidate();   /* capture size changes with the scale */
    if (s_tgt_ready) {
        int x0 = s_tx0, y0 = s_ty0, x1 = s_tx0 + s_tw - 1, y1 = s_ty0 + s_th - 1;
        tgt_destroy();
        tgt_create(x0, y0, x1, y1);
    }
    s_gpu_dirty = 1;
}
static int  vkb_scale(void) { return s_scale; }
static void vkb_set_texture_filter(int b) { s_texfilter = b ? 1 : 0; }
static int  vkb_texture_filter(void) { return s_texfilter; }

static void vkb_set_draw_area(int x1, int y1, int x2, int y2) {
    /* Scissor is per-batch (one render pass uses one scissor), so drain both
     * independent geometry batches before the clip rect changes. Otherwise
     * earlier textured GP0 primitives render with the next draw area's scissor. */
    if (x1 != s_da_x1 || y1 != s_da_y1 || x2 != s_da_x2 || y2 != s_da_y2) {
        flush_tex_batch();
        flush_geometry();
    }
    s_da_x1 = x1; s_da_y1 = y1; s_da_x2 = x2; s_da_y2 = y2;
}
static void vkb_get_draw_area(int *x1, int *y1, int *x2, int *y2) {
    if (x1) *x1 = s_da_x1; if (y1) *y1 = s_da_y1; if (x2) *x2 = s_da_x2; if (y2) *y2 = s_da_y2;
}
static void vkb_set_draw_offset(int x, int y) { s_off_x = x; s_off_y = y; }

/* GP0 02h fill: set a VRAM rect to a solid 1555 colour. The write lands in
 * the CPU array immediately (like every CPU-side VRAM write) and rides the
 * batched s_up_rects upload — the old per-fill staging upload was part of
 * the per-op alloc/submit churn. */
/* CPU-path fill: guest VRAM + the upload queue (texture/CPU writes). */
static void vkb_fill_rect_guest(int x, int y, int w, int h, uint16_t color) {
    sw_fill_rect(x, y, w, h, color);
    if (!s_ctx_ok) return;
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > VRAM_W) w = VRAM_W - x; if (y + h > VRAM_H) h = VRAM_H - y;
    if (w <= 0 || h <= 0) return;
    up_add(x, y, x + w - 1, y + h - 1);
}

/* GP0 0x02 fill is a DRAW, not an upload: inside the wide framebuffer's
 * overflow columns ([video] vram_extension) it goes to the target and lands in
 * the extension plane through the pack, never in the guest texture pages. */
static void vkb_fill_rect(int x, int y, int w, int h, uint16_t color) {
    int gx0, gy0, gx1, gy1;
    if (w <= 0 || h <= 0) return;
    if (vram_ext_region(&gx0, &gy0, &gx1, &gy1)) {
        int ox0 = x > VRAM_EXT_X0 ? x : VRAM_EXT_X0;
        int ox1 = (x + w - 1) < gx1 ? (x + w - 1) : gx1;
        int oy0 = y > gy0 ? y : gy0;
        int oy1 = (y + h - 1) < gy1 ? (y + h - 1) : gy1;
        if (ox0 <= ox1 && oy0 <= oy1) {
            if (x < ox0) vkb_fill_rect_guest(x, y, ox0 - x, h, color);
            if (y < oy0)
                vkb_fill_rect_guest(ox0, y, ox1 - ox0 + 1, oy0 - y, color);
            if (oy1 + 1 < y + h)
                vkb_fill_rect_guest(ox0, oy1 + 1, ox1 - ox0 + 1,
                                    (y + h) - (oy1 + 1), color);
            vkb_draw_flat_rect(ox0, oy0, ox1 - ox0 + 1, oy1 - oy0 + 1, color);
            return;
        }
    }
    vkb_fill_rect_guest(x, y, w, h, color);
}

/* Same policy as GL: skip framebuffer-sized depth24 (RGB888) transfers only;
 * still stage smaller texture A0s. On leave clear skipped FB union — never
 * restage packed RGB888 as 1555. */
static int s_depth24_skip_up = 0;
static DirtyRect s_d24_skip_fb;

static int depth24_is_fb_transfer(int x, int y, int w, int h) {
    if (!gpu_display_is_depth24() || w <= 0 || h <= 0) return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    int fb_w = (int)((di.width * 3u + 1u) / 2u);
    int fb_h = (int)di.height;
    if (fb_w < 8) fb_w = 8;
    if (fb_h < 1) fb_h = 1;
    /* See GL: 256×256 pages must not match the half-FB area heuristic. */
    if (w <= 256 && h <= 256) return 0;
    {
        int dx = (int)(di.display_x & 1023u);
        int dy = (int)(di.display_y & 511u);
        int x0 = x & (VRAM_W - 1), y0 = y & (VRAM_H - 1);
        if (x0 + w <= dx || x0 >= dx + fb_w || y0 + h <= dy || y0 >= dy + fb_h)
            return 0;
    }
    if (h >= fb_h - 8 && h <= fb_h + 16 && w >= (fb_w * 3) / 4) return 1;
    if ((int64_t)w * (int64_t)h >= ((int64_t)fb_w * fb_h) / 2) return 1;
    return 0;
}

static void depth24_mark_scanout_band(void) {
    GpuDisplayInfo di;
    int fb_w, fb_h, x0, y0, x1, y1;
    if (!gpu_display_is_depth24()) return;
    gpu_get_display_info(&di);
    fb_w = (int)((di.width * 3u + 1u) / 2u);
    fb_h = (int)di.height;
    if (fb_w < 8) fb_w = 8;
    if (fb_h < 1) fb_h = 1;
    x0 = (int)(di.display_x & 1023u);
    y0 = (int)(di.display_y & 511u);
    x1 = x0 + fb_w - 1;
    y1 = y0 + fb_h - 1;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    rect_add(&s_d24_skip_fb, x0, y0, x1, y1);
}

static void depth24_clear_skipped_fb(void) {
    /* GL scissor-clears the skipped FB union. VK keeps texture uploads that
     * landed during depth24; the FB bands are overwritten by the next 15-bit
     * draws. (A full-image ClearColorImage would wipe those textures.) */
    rect_clear(&s_d24_skip_fb);
}

static void depth24_upload_policy(void) {
    int d24 = gpu_display_is_depth24();
    if (d24 && !s_depth24_skip_up) {
        s_up_nrects = 0;
        rect_clear(&s_d24_skip_fb);
    } else if (!d24 && s_depth24_skip_up) {
        s_up_nrects = 0;
        depth24_clear_skipped_fb();
        gpu_depth24_upload_span_reset();
    }
    s_depth24_skip_up = d24;
}

static void vkb_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data) {
    sw_vram_transfer_in(x, y, w, h, data);
    if (!s_ctx_ok) return;
    depth24_upload_policy();
    if (s_depth24_skip_up && depth24_is_fb_transfer(x, y, w, h)) {
        /* Full-VRAM savestate restore: stage FBO; mark scanout band only. */
        if (w >= VRAM_W && h >= VRAM_H) {
            up_add_transfer(x, y, w, h);
            rect_clear(&s_d24_skip_fb);
            depth24_mark_scanout_band();
            return;
        }
        depth24_mark_scanout_band();
        return;
    }
    up_add_transfer(x, y, w, h);   /* exact touched rects, incl. per-pixel wrap */
}

/* The [video] vram_extension plane is host scratch, not guest state: after a
 * savestate restore the restored frame must not show the previous session's
 * overflow columns, so clear both the CPU plane and the raw mirror region. */
static void vkb_ext_clear(void) {
    if (!s_ready || !s_ctx_ok) return;
    const size_t bytes = (size_t)VRAM_EXT_COLS * VRAM_H * 2;
    memset(vram_ext_host(), 0, bytes);
    VkBuffer buf; VkDeviceMemory mem; void *map;
    if (!make_staging((VkDeviceSize)bytes, &buf, &mem, &map)) return;
    memset(map, 0, bytes);
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy rc = {0};
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rc.imageSubresource.layerCount = 1;
    rc.imageOffset.x = VRAM_EXT_HOST_X0;   /* 1024, not VRAM_EXT_OFFSET: the
                                            * guest texture columns 512..703
                                            * must not be zeroed */
    rc.imageExtent.width = VRAM_EXT_COLS;
    rc.imageExtent.height = VRAM_H;
    rc.imageExtent.depth = 1;
    p_vkCmdCopyBufferToImage(cb, buf, s_raw_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_oneshot(cb);
    defer_staging(buf, mem);
}

void vk_renderer_restage_vram_after_savestate(void) {
    if (!s_ctx_ok || !s_vram) return;
    ri_invalidate();   /* a restored frame must not crossfade the old one */
    s_up_nrects = 0;
    rect_clear(&s_d24_skip_fb);
    s_depth24_skip_up = 0;
    up_add_transfer(0, 0, VRAM_W, VRAM_H);
    flush_cpu_upload();
    vkb_ext_clear();
    if (gpu_display_is_depth24()) {
        s_depth24_skip_up = 1;
        depth24_mark_scanout_band();
    }
}
static void vkb_vram_transfer_out(int x, int y, int w, int h, uint16_t *data) {
    ensure_cpu();   /* sync GPU-rendered content down to the CPU mirror first */
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            data[row * w + col] = s_vram ? s_vram[(y + row) * VRAM_W + (x + col)] : 0;
}
static void vkb_vram_write(int x, int y, uint16_t pixel) {
    sw_vram_write(x, y, pixel);
    if (!s_ctx_ok) return;
    depth24_upload_policy();
    up_add(x & (VRAM_W - 1), y & (VRAM_H - 1),
           x & (VRAM_W - 1), y & (VRAM_H - 1));
}
static uint16_t vkb_vram_read(int x, int y) {
    ensure_cpu();   /* gated by s_gpu_dirty; cheap when nothing was drawn */
    return s_vram ? s_vram[y * VRAM_W + x] : 0;
}
/* Scanout read ([video] vram_extension): the wide framebuffer's overflow
 * columns are in the extension plane, everything else in guest VRAM. */
static uint16_t vkb_vram_read_display(int x, int y) {
    ensure_cpu();
    int i = vram_ext_plane_index(x, y);
    if (i >= 0) return vram_ext_host()[i];
    return s_vram ? s_vram[y * VRAM_W + x] : 0;
}

/* render_display: CPU readout path (screenshots / debug server / the 24-bit
 * FMV present). Sync the GPU-authoritative VRAM down, then read out via the
 * software renderer on the shared array — mirrors the GL backend exactly. */
static int vkb_render_display(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    ensure_cpu();
    return sw_render_display(o, p, dx, dy, dw, dh);
}
static int vkb_render_display_hires(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    ensure_cpu();
    return sw_render_display_hires(o, p, dx, dy, dw, dh);
}

/* ---- geometry batch (Phase 2) ------------------------------------------ */
/* Set the full-image viewport and the draw-area scissor (in framebuffer px). */
static void set_vp_scissor(VkCommandBuffer cb) {
    VkViewport vp; tgt_viewport(&vp);
    p_vkCmdSetViewport(cb, 0, 1, &vp);
    int x1 = s_da_x1 < 0 ? 0 : s_da_x1, y1 = s_da_y1 < 0 ? 0 : s_da_y1;
    int x2 = s_da_x2 >= VRAM_W ? VRAM_W - 1 : s_da_x2;
    int y2 = s_da_y2 >= VRAM_H ? VRAM_H - 1 : s_da_y2;
    set_scissor_px(cb, x1, y1, x2 - x1 + 1, y2 - y1 + 1);
}

/* ---- native-wide compositor helpers ------------------------------------- */
/* X-translation (native px) from canonical VRAM space into the active wide
 * surface: local_x = vram_x - base_x + OFFSET. Same as GL/SW wide_dx(). */
static int view_enabled, view_shift, view_pad_left, view_pad_right;
static int wide_dx(void) { return s_wide_offset + view_shift - s_wide_cur_base; }

static void wide_free_all(void) {
    int any = 0;
    for (int i = 0; i < VK_WIDE_MAX_SURF; i++) any |= (s_wide_img[i] != VK_NULL_HANDLE);
    if (any) p_vkQueueWaitIdle(s_queue);   /* surfaces may be referenced in flight */
    for (int i = 0; i < VK_WIDE_MAX_SURF; i++) {
        if (s_wide_fb[i])      { p_vkDestroyFramebuffer(s_dev, s_wide_fb[i], NULL); s_wide_fb[i] = VK_NULL_HANDLE; }
        if (s_wide_view[i])    { p_vkDestroyImageView(s_dev, s_wide_view[i], NULL); s_wide_view[i] = VK_NULL_HANDLE; }
        if (s_wide_img[i])     { p_vkDestroyImage(s_dev, s_wide_img[i], NULL); s_wide_img[i] = VK_NULL_HANDLE; }
        if (s_wide_mem[i])     { p_vkFreeMemory(s_dev, s_wide_mem[i], NULL); s_wide_mem[i] = VK_NULL_HANDLE; }
        if (s_wide_ds_view[i]) { p_vkDestroyImageView(s_dev, s_wide_ds_view[i], NULL); s_wide_ds_view[i] = VK_NULL_HANDLE; }
        if (s_wide_ds_img[i])  { p_vkDestroyImage(s_dev, s_wide_ds_img[i], NULL); s_wide_ds_img[i] = VK_NULL_HANDLE; }
        if (s_wide_ds_mem[i])  { p_vkFreeMemory(s_dev, s_wide_ds_mem[i], NULL); s_wide_ds_mem[i] = VK_NULL_HANDLE; }
        s_wide_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        s_wide_base[i] = -1;
    }
    s_wide_cur = -1;
}

/* Find (or lazily create) the wide surface for base_x. Returns the surface
 * index or -1. A new surface = RGBA8 color image (attachment + blit src/dst)
 * + its own stencil image + a framebuffer on the SHARED render pass; color is
 * cleared black, stencil to 0, then both are parked in the layouts the render
 * pass expects (color/DS ATTACHMENT_OPTIMAL). */
static int wide_surf_for(int base_x) {
    if (s_wide_w <= 0 || !s_ready) return -1;
    for (int i = 0; i < VK_WIDE_MAX_SURF; i++)
        if (s_wide_img[i] && s_wide_base[i] == base_x) return i;
    for (int i = 0; i < VK_WIDE_MAX_SURF; i++) {
        if (s_wide_img[i]) continue;
        int S = s_scale, w = s_wide_w * S, h = VRAM_H * S;
        if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &s_wide_img[i], &s_wide_mem[i], &s_wide_view[i]))
            return -1;
        if (!make_image(s_ds_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        DS_ASPECT, &s_wide_ds_img[i], &s_wide_ds_mem[i], &s_wide_ds_view[i]))
            return -1;
        VkImageView views[2] = { s_wide_view[i], s_wide_ds_view[i] };
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = s_rpass; fi.attachmentCount = 2; fi.pAttachments = views;
        fi.width = (uint32_t)w; fi.height = (uint32_t)h; fi.layers = 1;
        if (p_vkCreateFramebuffer(s_dev, &fi, NULL, &s_wide_fb[i]) != VK_SUCCESS)
            return -1;
        /* Clear + park in attachment layouts (mirrors the s_ds init chain). */
        VkCommandBuffer cb = begin_oneshot();
        s_wide_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black = {{0, 0, 0, 0}};
        VkImageSubresourceRange crng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        p_vkCmdClearColorImage(cb, s_wide_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &crng);
        img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; db.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = s_wide_ds_img[i]; db.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        db.subresourceRange.aspectMask = DS_ASPECT;
        db.subresourceRange.levelCount = 1; db.subresourceRange.layerCount = 1;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, NULL, 0, NULL, 1, &db);
        VkClearDepthStencilValue dsv = { 0.0f, 0 };
        VkImageSubresourceRange srng = { DS_ASPECT, 0, 1, 0, 1 };
        p_vkCmdClearDepthStencilImage(cb, s_wide_ds_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dsv, 1, &srng);
        db.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        db.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        db.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        db.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &db);
        end_oneshot(cb);
        s_wide_base[i] = base_x;
        return i;
    }
    return -1;  /* more distinct buffers than VK_WIDE_MAX_SURF — shouldn't happen */
}

/* Begin the mirror render pass on the active wide surface inside an already-
 * recording one-shot CB (canonical pass already ended). Sets the wide viewport
 * and the mirror scissor: FULL width X (the margins are the point) but the
 * DRAW-AREA Y band — native-wide only widens X; a full-height Y let draws bleed
 * across a vertical double-buffer band boundary (the GL band-flicker lesson). */
static void wide_pass_begin(VkCommandBuffer cb) {
    int i = s_wide_cur, S = s_scale;
    s_perf_cur.wide_passes++;
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    /* Stencil self-barrier for the WIDE DS image (same hazard class as
     * begin_geo_pass covers for the canonical DS across one-shot submits). */
    VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    db.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    db.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = s_wide_ds_img[i];
    db.subresourceRange.aspectMask = DS_ASPECT;
    db.subresourceRange.levelCount = 1; db.subresourceRange.layerCount = 1;
    db.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, NULL, 0, NULL, 1, &db);
    color_self_barrier(cb, s_wide_img[i]);
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = s_rpass; rp.framebuffer = s_wide_fb[i];
    rp.renderArea.extent.width = (uint32_t)(s_wide_w * S);
    rp.renderArea.extent.height = (uint32_t)(VRAM_H * S);
    p_vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0.f, 0.f, (float)(s_wide_w * S), (float)(VRAM_H * S), 0.f, 1.f };
    p_vkCmdSetViewport(cb, 0, 1, &vp);
    int y1 = s_da_y1 < 0 ? 0 : s_da_y1;
    int y2 = s_da_y2 >= VRAM_H ? VRAM_H - 1 : s_da_y2;
    int hh = y2 - y1 + 1; if (hh < 0) hh = 0;
    VkRect2D sc = { { view_pad_left * S, y1 * S },
        { (uint32_t)((s_wide_w - view_pad_left - view_pad_right) * S), (uint32_t)(hh * S) } };
    p_vkCmdSetScissor(cb, 0, 1, &sc);
}

/* Tight bbox of the open untextured batch (added to s_pack_dirty at flush). */
static DirtyRect s_geo_bbox;
/* Same for the open textured batch (drives the windowed-target growth check). */
static DirtyRect s_tex_bbox;

/* Flush accumulated untextured geometry: one render pass draws the whole batch
 * (blend/mask per the batch keys), then the image returns to TRANSFER_SRC. */
static void flush_geometry(void) {
    if (s_vcount == 0 || !s_ready) return;
    /* Grow the windowed target BEFORE the batch is consumed: tgt_require may
     * drain the queue (which recycles the sub-allocation cursors while the
     * open batch is still at s_vbase). */
    if (s_geo_bbox.set) {
        int gx0 = s_geo_bbox.x0 < s_da_x1 ? s_da_x1 : s_geo_bbox.x0;
        int gy0 = s_geo_bbox.y0 < s_da_y1 ? s_da_y1 : s_geo_bbox.y0;
        int gx1 = s_geo_bbox.x1 > s_da_x2 ? s_da_x2 : s_geo_bbox.x1;
        int gy1 = s_geo_bbox.y1 > s_da_y2 ? s_da_y2 : s_geo_bbox.y1;
        perf_cover(gx0, gy0, gx1, gy1);
        tgt_require_write(gx0, gy0, gx1, gy1);
        if (!s_tgt_ready) { s_vcount = 0; s_geo_bbox.set = 0; return; }
    }
    s_perf_cur.geo_flushes++;
    uint32_t base = s_vbase, n = s_vcount;
    s_vbase += n; s_vcount = 0;   /* consumed region stays untouched until gpu_sync */
    int semi = s_geo_semi, blend = (semi < 0) ? 0 : (semi + 1);
    VkCommandBuffer cb = begin_oneshot();
    vram_to(cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_geo_pass(cb);
    set_vp_scissor(cb);
    bind_masked(cb, 0, 0, blend, s_geo_check, s_geo_mask);
    GeoPush gp = { px_shift(), 0.0f, 512.0f, s_geo_fog };
    p_vkCmdPushConstants(cb, s_pl_geo, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof gp, &gp);
    VkDeviceSize off = 0;
    p_vkCmdBindVertexBuffers(cb, 0, 1, &s_vbuf, &off);
    perf_draw(cb, n, 1, base, 0);
    p_vkCmdEndRenderPass(cb);
    if (s_wide_cur >= 0 && !s_geo_mirror_suppress) {   /* native-wide mirror pass (same CB) */
        wide_pass_begin(cb);
        bind_masked(cb, 0, 0, blend, s_geo_check, s_geo_mask);
        GeoPush wgp = { px_shift(), (float)wide_dx(), (float)s_wide_w / 2.0f, s_geo_fog };
        p_vkCmdPushConstants(cb, s_pl_geo, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof wgp, &wgp);
        perf_draw(cb, n, 1, base, 0);
        p_vkCmdEndRenderPass(cb);
    }
    s_geo_mirror_suppress = 0;
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
    if (s_geo_bbox.set) {
        int x0 = s_geo_bbox.x0 < s_da_x1 ? s_da_x1 : s_geo_bbox.x0;
        int y0 = s_geo_bbox.y0 < s_da_y1 ? s_da_y1 : s_geo_bbox.y0;
        int x1 = s_geo_bbox.x1 > s_da_x2 ? s_da_x2 : s_geo_bbox.x1;
        int y1 = s_geo_bbox.y1 > s_da_y2 ? s_da_y2 : s_geo_bbox.y1;
        rect_add(&s_pack_dirty, x0, y0, x1, y1);
        rect_clear(&s_geo_bbox);
    }
    s_gpu_dirty = 1;
}

static inline void col555(uint16_t c, float out[3]) {
    out[0] = ((c)       & 0x1F) / 31.0f;
    out[1] = ((c >> 5)  & 0x1F) / 31.0f;
    out[2] = ((c >> 10) & 0x1F) / 31.0f;
}
static inline void col888(uint32_t c, float out[3]) {
    out[0] = ( c        & 0xFF) / 255.0f;
    out[1] = ((c >> 8)  & 0xFF) / 255.0f;
    out[2] = ((c >> 16) & 0xFF) / 255.0f;
}
static inline void push_vert(float x, float y, const float rgb[3]) {
    Vert *v = &s_vmap[s_vbase + s_vcount++];
    v->x = x; v->y = y; v->r = rgb[0]; v->g = rgb[1]; v->b = rgb[2];
    v->a = (float)s_geo_mask;
    rect_add(&s_geo_bbox, (int)floorf(x), (int)floorf(y), (int)ceilf(x), (int)ceilf(y));
}
/* Ensure room for `need` verts (flush mid-batch only between primitives). */
static void ensure_room(uint32_t need) {
    if (s_vbase + s_vcount + need > VK_VBUF_VERTS) {
        flush_geometry();
        if (s_vbase + need > VK_VBUF_VERTS)
            gpu_sync();   /* buffer exhausted: wait idle, cursors recycle to 0 */
    }
}
/* Open / continue an untextured batch with the current blend/mask state; drains
 * the textured batch and any differently-keyed untextured batch first (PS1
 * submission order). */
static void geo_prim_begin(void) {
    flush_cpu_upload();   /* pending CPU writes precede this prim (PS1 order) */
    flush_tex_batch();
    int semi = s_semi_en ? s_semi_mode : -1;
    if (s_vcount > 0 && (semi != s_geo_semi || s_mask_set != s_geo_mask || s_mask_check != s_geo_check))
        flush_geometry();
    if (s_vcount == 0) { s_geo_semi = semi; s_geo_mask = s_mask_set; s_geo_check = s_mask_check;
                         s_geo_fog = vfx_prim_fog(); }
}

static void tri3(float ax,float ay,const float ca[3],
                 float bx,float by,const float cb_[3],
                 float cx,float cy,const float cc[3]) {
    if (!s_ready) return;
    ensure_room(3);
    push_vert(ax, ay, ca); push_vert(bx, by, cb_); push_vert(cx, cy, cc);
}
static void quad(float x0,float y0,float x1,float y1,float x2,float y2,float x3,float y3,
                 const float c[3]) {
    /* (x0,y0)-(x1,y1)-(x2,y2)-(x3,y3) as two tris */
    tri3(x0,y0,c, x1,y1,c, x2,y2,c);
    tri3(x0,y0,c, x2,y2,c, x3,y3,c);
}

/* Per-prim render state. Batch keys are compared at draw-append time (the draw
 * funcs flush when they change), so the setters only record state. */
static void vkb_set_semi_transparency(int e, int m) { s_semi_en = e ? 1 : 0; s_semi_mode = m & 3; }
static void vkb_set_mask_bits(int s, int c) { s_mask_set = s ? 1 : 0; s_mask_check = c ? 1 : 0; }
static void vkb_set_texture_window(uint32_t raw) {
    s_tw_mask_x = (int)(raw & 0x1F);        s_tw_mask_y = (int)((raw >> 5) & 0x1F);
    s_tw_off_x  = (int)((raw >> 10) & 0x1F); s_tw_off_y  = (int)((raw >> 15) & 0x1F);
}
static void vkb_set_color_modulation(int r, int g, int b, int raw) {
    s_mod_r = r; s_mod_g = g; s_mod_b = b; s_mod_raw = raw ? 1 : 0;
}

/* Sub-pixel / perspective overrides for the next triangle. */
static void vkb_set_precise_triangle(int enabled,
                                     int32_t x0,int32_t y0, int32_t x1,int32_t y1,
                                     int32_t x2,int32_t y2) {
    s_pc_valid = enabled ? 1 : 0;
    if (s_pc_valid) {
        /* 16.16 native VRAM px -> float; the VK pipeline is float end-to-end. */
        s_pc_x[0] = (float)x0 / 65536.0f;  s_pc_y[0] = (float)y0 / 65536.0f;
        s_pc_x[1] = (float)x1 / 65536.0f;  s_pc_y[1] = (float)y1 / 65536.0f;
        s_pc_x[2] = (float)x2 / 65536.0f;  s_pc_y[2] = (float)y2 / 65536.0f;
    }
}
static void vkb_set_perspective_triangle(int enabled, float q0, float q1, float q2) {
    s_pq_valid = (enabled && q0 > 0.0f && q1 > 0.0f && q2 > 0.0f) ? 1 : 0;
    s_pq[0] = q0; s_pq[1] = q1; s_pq[2] = q2;
    /* The corrected-triangle tally lives in the software rasterizer and backs
     * gpu_texture_correction_hits() — the "is this doing anything on this
     * title" counter. Forward so it reads the same on every backend. */
    sw_set_perspective_triangle(enabled, q0, q1, q2);
}
/* The override describes exactly one triangle; drop it once submitted so a
 * later prim can never inherit it. */
static inline void precise_consumed(void) { s_pc_valid = 0; s_pq_valid = 0; }

/* Position a vertex: the sub-pixel value when geometry_correction supplied one
 * for this triangle, else the integer GP0 position. */
#define PCX(i, v) (s_pc_valid ? s_pc_x[i] : (float)(v))
#define PCY(i, v) (s_pc_valid ? s_pc_y[i] : (float)(v))

static void vkb_draw_flat_triangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c){
    geo_prim_begin();
    float col[3]; col555(c, col);
    tri3(PCX(0,x0),PCY(0,y0),col, PCX(1,x1),PCY(1,y1),col, PCX(2,x2),PCY(2,y2),col);
    precise_consumed();
}
static void vkb_draw_gouraud_triangle(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1,int x2,int y2,uint16_t c2){
    geo_prim_begin();
    float a[3],b[3],cc[3]; col555(c0,a); col555(c1,b); col555(c2,cc);
    tri3(PCX(0,x0),PCY(0,y0),a, PCX(1,x1),PCY(1,y1),b, PCX(2,x2),PCY(2,y2),cc);
    precise_consumed();
}
/* Full-screen-overlay wide pass (pause gray-filter / load fade): draw a flat
 * rect covering the FULL wide width [0, wide_w) x [y, y+h) directly into the
 * active wide surface (positions already wide-space, u_xoff = 0). Mirrors GL's
 * wide_flat_rect_direct: full wide width (the overlay must dim the revealed
 * margins too) but only the draw-area rows, like SW rt_wide() — a full-height
 * scissor let a rect that canonically clips at a vertical double-buffer band
 * boundary paint the OTHER band's rows. The 6 verts are consumed privately (s_vbase bumped)
 * so they never join a canonical batch. */
static void wide_overlay_rect(int y, int h, uint16_t c, int semi) {
    if (s_wide_cur < 0 || !s_ready) return;
    ensure_room(6);
    float col[3]; col555(c, col);
    uint32_t base = s_vbase + s_vcount;   /* == s_vbase (batches drained) */
    float x0 = 0.0f, x1 = (float)s_wide_w, fy0 = (float)y, fy1 = (float)(y + h);
    push_vert(x0, fy0, col); push_vert(x1, fy0, col); push_vert(x1, fy1, col);
    push_vert(x0, fy0, col); push_vert(x1, fy1, col); push_vert(x0, fy1, col);
    s_vbase += s_vcount; s_vcount = 0;    /* consume privately */
    int blend = (semi < 0) ? 0 : (semi + 1);
    VkCommandBuffer cb = begin_oneshot();
    wide_pass_begin(cb);   /* band scissor: full wide width, draw-area rows */
    bind_masked(cb, 0, 0, blend, s_mask_check, s_mask_set);
    GeoPush gp = { px_shift(), 0.0f, (float)s_wide_w / 2.0f, s_geo_fog };
    p_vkCmdPushConstants(cb, s_pl_geo, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof gp, &gp);
    VkDeviceSize off = 0;
    p_vkCmdBindVertexBuffers(cb, 0, 1, &s_vbuf, &off);
    perf_draw(cb, 6, 1, base, 0);
    p_vkCmdEndRenderPass(cb);
    end_oneshot(cb);
}

static void vkb_draw_flat_rect(int x,int y,int w,int h,uint16_t c){
    /* Full-screen 2D overlay under native-wide: the canonical rect must NOT
     * mirror 1:1 (it would cover only the translated 4:3 span) — suppress the
     * batch mirror and emit one full-wide-width rect instead, so the revealed
     * margins are dimmed/faded too. Same detection as GL/SW. */
    int overlay = 0;
    if (s_wide_cur >= 0) {
        int native_w = s_wide_w - 2 * s_wide_offset;
        int lx = x - s_wide_cur_base, rx = x + w - s_wide_cur_base;
        overlay = (native_w > 0 && lx <= 0 && rx >= native_w);
    }
    if (overlay) { flush_tex_batch(); flush_geometry(); }
    geo_prim_begin();
    float col[3]; col555(c, col);
    if (overlay) s_geo_mirror_suppress = 1;
    quad((float)x,(float)y,(float)(x+w),(float)y,(float)(x+w),(float)(y+h),(float)x,(float)(y+h),col);
    if (overlay) {
        int semi = s_semi_en ? s_semi_mode : -1;
        flush_geometry();                  /* canonical only (mirror suppressed) */
        wide_overlay_rect(y, h, c, semi);
    }
}
/* 1px line as a thin quad along the segment. */
static void line_quad(int x0,int y0,int x1,int y1,const float ca[3],const float cb_[3]){
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float len = dx*dx + dy*dy;
    float nx = 0.5f, ny = 0.5f;
    if (len > 0.0001f) { float il = 0.5f / (float)sqrt(len); nx = -dy * il; ny = dx * il; }
    ensure_room(6);
    float ax0 = x0 + nx, ay0 = y0 + ny, bx0 = x0 - nx, by0 = y0 - ny;
    float ax1 = x1 + nx, ay1 = y1 + ny, bx1 = x1 - nx, by1 = y1 - ny;
    tri3(ax0,ay0,ca, ax1,ay1,cb_, bx1,by1,cb_);
    tri3(ax0,ay0,ca, bx1,by1,cb_, bx0,by0,ca);
}
static void vkb_draw_line(int x0,int y0,int x1,int y1,uint16_t c){
    geo_prim_begin();
    float col[3]; col555(c, col); line_quad(x0,y0,x1,y1,col,col);
}
static void vkb_draw_shaded_line(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1){
    geo_prim_begin();
    float a[3],b[3]; col555(c0,a); col555(c1,b); line_quad(x0,y0,x1,y1,a,b);
}

/* ---- textured primitives + VRAM->VRAM copy (Phase 3) ------------------- */
/* Flush the textured batch: two STP-split passes (pass 1 opaque texels, pass 2
 * semi-transparent texels with PS1 blending). Per-prim texture state rode in the
 * vertex; the batch keys (semi/mask/twin/filter) drive the push constants and
 * pipeline. The raw mirror was made coherent at append time (flush_pack_if_sampling). */
static void flush_tex_batch(void) {
    if (s_tcount == 0 || !s_ready) return;
    if (s_tex_bbox.set) {
        perf_cover(s_tex_bbox.x0, s_tex_bbox.y0, s_tex_bbox.x1, s_tex_bbox.y1);
        tgt_require_write(s_tex_bbox.x0, s_tex_bbox.y0, s_tex_bbox.x1, s_tex_bbox.y1);
        rect_clear(&s_tex_bbox);
        if (!s_tgt_ready) { s_tcount = 0; return; }
    }
    s_perf_cur.tex_flushes++;
    uint32_t base = s_tbase, n = s_tcount;
    s_tbase += n; s_tcount = 0;   /* consumed region stays untouched until gpu_sync */
    int semi = s_tb_semi, blend = (semi < 0) ? 0 : (semi + 1);
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vram_to(cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_geo_pass(cb);
    set_vp_scissor(cb);
    VkDescriptorSet tex_set = s_tb_hd_set ? s_tb_hd_set : s_ds_tex;
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl_tex, 0, 1, &tex_set, 0, NULL);
    VkDeviceSize off = 0;
    p_vkCmdBindVertexBuffers(cb, 0, 1, &s_tbuf, &off);
    TexPush tp = {0};
    tp.shift = px_shift(); tp.xoff = 0.0f; tp.xhalf = 512.0f;
    tp.maskset = s_tb_mask; tp.filter = s_tb_filter;
    tp.fog_z = s_tb_fog;   /* [video] volumetrics: < 0 = off */
    tp.twin[0] = s_tb_twin[0]; tp.twin[1] = s_tb_twin[1]; tp.twin[2] = s_tb_twin[2]; tp.twin[3] = s_tb_twin[3];
    if (semi < 0) {
        /* OPAQUE batch: the STP bit does not gate COLOUR (every texel is
         * opaque), so colour draws in ONE ordered pass over all texels —
         * painter's order across the batch is exact — followed by a
         * COLOUR-MASKED pass that only fixes the STENCIL for STP=1 texels.
         * (The old whole-batch STP-split let a behind prim's STP=1 texels
         * paint OVER a front prim's STP=0 colour — the water-sparkle field
         * speckled every hill in the Tomba2 attract demo. Mirrors GL's
         * tex_batch_draw_passes.) */
        bind_masked(cb, 1, 0, 0, s_tb_check, s_tb_mask);
        tp.semipass = 0;
        p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
        perf_draw(cb, n, 1, base, 0);
        bind_masked_stencil_only(cb, 1, 0, s_tb_check, 1);
        tp.semipass = 2;
        p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
        perf_draw(cb, n, 1, base, 0);
    } else {
        /* SEMI batch: genuine two-pass — STP=0 texels opaque, STP=1 texels
         * blended per the PSX mode. Cross-prim order is kept by isolating
         * semi prims to one per batch (gpu_textured_triangle), so the two
         * passes never straddle overlapping prims. */
        bind_masked(cb, 1, 0, 0, s_tb_check, s_tb_mask);
        tp.semipass = 1;
        p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
        perf_draw(cb, n, 1, base, 0);
        bind_masked(cb, 1, 0, blend, s_tb_check, 1);
        tp.semipass = 2;
        p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
        perf_draw(cb, n, 1, base, 0);
    }
    p_vkCmdEndRenderPass(cb);
    if (s_wide_cur >= 0) {                 /* native-wide mirror pass (same CB) */
        wide_pass_begin(cb);
        tp.xoff = (float)wide_dx(); tp.xhalf = (float)s_wide_w / 2.0f;
        if (semi < 0) {
            bind_masked(cb, 1, 0, 0, s_tb_check, s_tb_mask);
            tp.semipass = 0;
            p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
            perf_draw(cb, n, 1, base, 0);
            bind_masked_stencil_only(cb, 1, 0, s_tb_check, 1);
            tp.semipass = 2;
            p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
            perf_draw(cb, n, 1, base, 0);
        } else {
            bind_masked(cb, 1, 0, 0, s_tb_check, s_tb_mask);
            tp.semipass = 1;
            p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
            perf_draw(cb, n, 1, base, 0);
            bind_masked(cb, 1, 0, blend, s_tb_check, 1);
            tp.semipass = 2;
            p_vkCmdPushConstants(cb, s_pl_tex, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof tp, &tp);
            perf_draw(cb, n, 1, base, 0);
        }
        p_vkCmdEndRenderPass(cb);
    }
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    end_oneshot(cb);
    s_gpu_dirty = 1;
}

/* Shared PS1 uv-sampling model (limits + mirrored-2D compensation) — one
 * implementation for GL/VK/SW, see gpu_uv.h. */
#include "gpu_uv.h"

/* Append a textured triangle to the batch (per-prim state in the vertex; flush
 * first if the batch keys differ or the raw mirror needs packing). col = 3x RGB
 * modulation per vertex; lim = uv bounds (NULL -> derive). */
static void gpu_textured_triangle(const int *xs, const int *ys, const int *us, const int *vs,
                                  const float *col, uint16_t texpage,
                                  uint16_t clut_x, uint16_t clut_y, int rawtex,
                                  int semi, const int *lim) {
    if (!s_ready) return;
    int lim_buf[4];
    int uv_buf[6];
    if (!lim) {
        /* Poly path: exact sampled bounds from the ORIGINAL uvs, then the
         * center-sampling mirror compensation (rect prims arrive with their
         * own precomputed lim and pre-bumped uvs). */
        int *mu = uv_buf, *mv = uv_buf + 3;
        for (int i = 0; i < 3; i++) { mu[i] = us[i]; mv[i] = vs[i]; }
        psx_uv_tri_center_sample(xs, ys, mu, mv, s_pq_valid, lim_buf);
        us = mu; vs = mv;
        lim = lim_buf;
    }
    int base_x = (texpage & 0xF) * 64;
    int base_y = ((texpage >> 4) & 1) * 256;
    int depth  = (texpage >> 7) & 3; if (depth > 2) depth = 2;

    flush_geometry();   /* drain untextured (PS1 order) */
    flush_cpu_upload(); /* uploaded texels/CLUTs must be sampleable + order */
    flush_pack_if_sampling(base_x, base_y, depth, clut_x, clut_y);

    /* HD pack override. The pack was matched against the tracked VRAM upload
     * when this texture page/CLUT arrived; here the (already decoded) image is
     * looked up in the GPU cache, so a repeated draw costs one key compare. */
    VkDescriptorSet hd_set = VK_NULL_HANDLE;
    float hd_src[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int hd_mode = 0;
    if (vk_hd_pack_active()) {
        GpuHdTextureImage hd = {0};
        uint32_t window = (uint32_t)(s_tw_mask_x | (s_tw_mask_y << 5) |
                                     (s_tw_off_x << 10) | (s_tw_off_y << 15));
        if (vk_hd_pack_acquire(texpage, clut_x, clut_y, lim, window, semi >= 0, &hd)) {
            hd_set = hd_texture(&hd, &hd_mode, hd_src);
            if (!hd_set) hd_mode = 0;
            else gpu_hd_textures_note_applied();   /* same counter as the GL path */
            vk_hd_pack_release(&hd);
        }
    }

    int twx = s_tw_mask_x, twy = s_tw_mask_y, tox = s_tw_off_x, toy = s_tw_off_y;
    /* A semi-transparent prim must NOT coalesce with its neighbours (its two
     * STP passes would straddle overlapping prims — painter's-order violation;
     * see flush_tex_batch). It drains the open batch and draws alone; the next
     * append flushes it out again. Opaque prims keep batching. */
    int isolate = (semi >= 0);
    if (s_tcount > 0 &&
        (isolate || semi != s_tb_semi || s_mask_set != s_tb_mask || s_mask_check != s_tb_check ||
         s_texfilter != s_tb_filter || hd_set != s_tb_hd_set ||
         twx != s_tb_twin[0] || twy != s_tb_twin[1] || tox != s_tb_twin[2] || toy != s_tb_twin[3]))
        flush_tex_batch();
    if (s_tbase + s_tcount + 3 > VK_TBUF_VERTS) {
        flush_tex_batch();
        if (s_tbase + 3 > VK_TBUF_VERTS)
            gpu_sync();   /* buffer exhausted: wait idle, cursors recycle to 0 */
    }
    if (s_tcount == 0) {
        s_tb_semi = semi; s_tb_mask = s_mask_set; s_tb_check = s_mask_check; s_tb_filter = s_texfilter;
        s_tb_twin[0] = twx; s_tb_twin[1] = twy; s_tb_twin[2] = tox; s_tb_twin[3] = toy;
        s_tb_hd_set = hd_set;
        s_tb_fog = vfx_prim_fog();   /* [video] volumetrics: batch depth proxy */
    }
    int bx0 = xs[0], bx1 = xs[0], by0 = ys[0], by1 = ys[0];
    for (int i = 0; i < 3; i++) {
        float *vp = s_tmap[s_tbase + s_tcount + i].v;
        vp[0] = s_pc_valid ? s_pc_x[i] : (float)xs[i];
        vp[1] = s_pc_valid ? s_pc_y[i] : (float)ys[i];
        vp[2] = (float)us[i];   vp[3] = (float)vs[i];
        vp[4] = col[i*3+0];     vp[5] = col[i*3+1];     vp[6] = col[i*3+2]; vp[7] = 1.0f;
        vp[8]  = (float)base_x;  vp[9]  = (float)base_y;
        vp[10] = (float)clut_x;  vp[11] = (float)clut_y;
        vp[12] = (float)depth;   vp[13] = (float)rawtex;
        vp[14] = (float)lim[0];  vp[15] = (float)lim[1];
        vp[16] = (float)lim[2];  vp[17] = (float)lim[3];
        vp[18] = s_pq_valid ? s_pq[i] : 0.0f;   /* a_q; 0 = affine */
        vp[19] = hd_src[0]; vp[20] = hd_src[1]; vp[21] = hd_src[2]; vp[22] = hd_src[3];
        vp[23] = (float)hd_mode;                /* 0 = native sampling */
        if (xs[i] < bx0) bx0 = xs[i]; if (xs[i] > bx1) bx1 = xs[i];
        if (ys[i] < by0) by0 = ys[i]; if (ys[i] > by1) by1 = ys[i];
    }
    s_tcount += 3;
    /* A sub-pixel-corrected vertex lies in [int, int+1) of the integer position
     * it was rounded from, so widen the readback rect by one pixel. */
    if (s_pc_valid) { bx1 += 1; by1 += 1; }
    /* mark dirty for later readback/pack (clamped to draw area) */
    if (bx0 < s_da_x1) bx0 = s_da_x1; if (by0 < s_da_y1) by0 = s_da_y1;
    if (bx1 > s_da_x2) bx1 = s_da_x2; if (by1 > s_da_y2) by1 = s_da_y2;
    rect_add(&s_pack_dirty, bx0, by0, bx1, by1);
    rect_add(&s_tex_bbox, bx0, by0, bx1, by1);
    s_gpu_dirty = 1;
}

static void gpu_textured_rect(int x,int y,int w,int h, int u0,int v0,int u1,int v1,
                              uint16_t clut_x,uint16_t clut_y,uint16_t tp,int semi) {
    if (w <= 0 || h <= 0) return;
    float mr = s_mod_r/255.0f, mg = s_mod_g/255.0f, mb = s_mod_b/255.0f;
    float col[9] = { mr,mg,mb, mr,mg,mb, mr,mg,mb };
    /* Mirrored quads arrive here as scaled rects with u0>u1 / v0>v1 (see
     * the GL backend): exact bounds from the original corners, then the
     * mirror bump (gpu_uv.h). */
    int lim[4];
    psx_uv_rect_limits(u0, v0, u1, v1, lim);
    psx_uv_rect_mirror_offset(&u0, &v0, &u1, &v1);
    int xs1[3]={x, x+w, x},    ys1[3]={y, y, y+h};
    int us1[3]={u0,u1,u0},     vs1[3]={v0,v0,v1};
    gpu_textured_triangle(xs1,ys1,us1,vs1,col,tp,clut_x,clut_y,s_mod_raw,semi,lim);
    int xs2[3]={x+w, x, x+w},  ys2[3]={y, y+h, y+h};
    int us2[3]={u1,u0,u1},     vs2[3]={v0,v1,v1};
    gpu_textured_triangle(xs2,ys2,us2,vs2,col,tp,clut_x,clut_y,s_mod_raw,semi,lim);
}

static void vkb_draw_textured_triangle(int x0,int y0,int u0,int v0,int x1,int y1,int u1,int v1,
                                       int x2,int y2,int u2,int v2,uint16_t cx,uint16_t cy,uint16_t tp){
    int xs[3]={x0,x1,x2}, ys[3]={y0,y1,y2}, us[3]={u0,u1,u2}, vs[3]={v0,v1,v2};
    float mr=s_mod_r/255.0f, mg=s_mod_g/255.0f, mb=s_mod_b/255.0f;
    float col[9]={mr,mg,mb, mr,mg,mb, mr,mg,mb};
    gpu_textured_triangle(xs,ys,us,vs,col,tp,cx,cy,s_mod_raw, s_semi_en?s_semi_mode:-1, NULL);
    precise_consumed();
}
static void vkb_draw_shaded_textured_triangle(int x0,int y0,int u0,int v0,uint32_t c0,
                                              int x1,int y1,int u1,int v1,uint32_t c1,
                                              int x2,int y2,int u2,int v2,uint32_t c2,
                                              uint16_t cx,uint16_t cy,uint16_t tp,int raw){
    int xs[3]={x0,x1,x2}, ys[3]={y0,y1,y2}, us[3]={u0,u1,u2}, vs[3]={v0,v1,v2};
    /* shaded-textured: per-vertex 24-bit colour modulates the texel (PS1 *2). */
    float col[9];
    col888(c0, &col[0]); col888(c1, &col[3]); col888(c2, &col[6]);
    gpu_textured_triangle(xs,ys,us,vs,col,tp,cx,cy,raw, s_semi_en?s_semi_mode:-1, NULL);
    precise_consumed();
}
static void vkb_draw_textured_rect(int x,int y,int w,int h,int u,int v,uint16_t cx,uint16_t cy,uint16_t tp){
    gpu_textured_rect(x,y,w,h, u,v, u+w,v+h, cx,cy,tp, s_semi_en?s_semi_mode:-1);
}
static void vkb_draw_textured_rect_scaled(int x,int y,int w,int h,int u0,int v0,int u1,int v1,uint16_t cx,uint16_t cy,uint16_t tp){
    gpu_textured_rect(x,y,w,h, u0,v0, u1,v1, cx,cy,tp, s_semi_en?s_semi_mode:-1);
}

/* VRAM->VRAM copy: blit the source hr region to scratch (resolves overlap),
 * then blit scratch -> dest with STP/mask split. Mirrors the GL backend.
 * A copy whose source or destination lies outside the windowed target takes
 * the CPU path (exact 1555 upload) — the raw mirror is the authoritative copy
 * there, so this stays correct and only costs on the rare off-window blit. */
static void vkb_copy_rect(int sx,int sy,int dx,int dy,int w,int h){
    if (!s_ctx_ok || w <= 0 || h <= 0) return;
    flush_cpu_upload();   /* the copy source may include pending CPU writes */
    flush_tex_batch(); flush_geometry();
    if (sx + w > VRAM_W) w = VRAM_W - sx;
    if (dx + w > VRAM_W) w = VRAM_W - dx;
    if (sy + h > VRAM_H) h = VRAM_H - sy;
    if (dy + h > VRAM_H) h = VRAM_H - dy;
    if (w <= 0 || h <= 0) return;
    s_perf_cur.copy_rects++;
    int S = s_scale;
    perf_cover(sx, sy, sx + w - 1, sy + h - 1);
    if (!tgt_contains(sx, sy, sx + w - 1, sy + h - 1) ||
        !tgt_contains(dx, dy, dx + w - 1, dy + h - 1)) {
        tgt_note_oob(dx, dy, dx + w - 1, dy + h - 1);
        if (s_vram) {   /* CPU mirror is the only complete native copy here */
            for (int row = 0; row < h; row++)
                for (int col = 0; col < w; col++)
                    s_vram[(dy + row) * VRAM_W + (dx + col)] =
                        s_vram[(sy + row) * VRAM_W + (sx + col)];
            up_add(dx, dy, dx + w - 1, dy + h - 1);
            flush_cpu_upload();
        }
        return;
    }

    /* stage 1: copy source hr region into scratch at the same coords */
    VkCommandBuffer cb = begin_oneshot();
    vram_to(cb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    img_to(cb, s_scratch_img, &s_scratch_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = {0};
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.layerCount = 1;
    ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.dstSubresource.layerCount = 1;
    ic.srcOffset.x = (sx - s_tx0) * S; ic.srcOffset.y = (sy - s_ty0) * S;
    ic.dstOffset.x = (sx - s_tx0) * S; ic.dstOffset.y = (sy - s_ty0) * S;
    ic.extent.width = w * S; ic.extent.height = h * S; ic.extent.depth = 1;
    p_vkCmdCopyImage(cb, s_vram_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     s_scratch_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    end_oneshot(cb);

    /* stage 2: blit scratch -> dest (hr-res source: src_div=1, offset (sx-dx)*S). */
    blit_region(dx, dy, w, h, s_scratch_view, s_scratch_img, &s_scratch_layout,
                1, (sx - dx) * S, (sy - dy) * S, 0, 1);

    if (s_vram) {   /* keep CPU mirror coherent for non-GPU readers */
        for (int row = 0; row < h; row++)
            for (int col = 0; col < w; col++)
                s_vram[(dy + row) * VRAM_W + (dx + col)] = s_vram[(sy + row) * VRAM_W + (sx + col)];
    }
}

/* ---- native-wide vtable entry points ------------------------------------ */
/* Enable native-wide with a wide width + centering offset (native px), or
 * disable (wide_w <= 0). No-ops when nothing changed (gpu.c calls this on
 * every draw-area set via ws_nw_sync_target — Part-A lesson: the wide entry
 * points sit inside guest emulation, keep the common path free). */
static void vkb_wide_set_view(int enabled, int shift, int pad_left, int pad_right) {
    if (!enabled) shift = pad_left = pad_right = 0;
    if (view_enabled == enabled && view_shift == shift &&
        view_pad_left == pad_left && view_pad_right == pad_right) return;
    flush_tex_batch(); flush_geometry();
    view_enabled = enabled;
    view_shift = shift;
    view_pad_left = pad_left;
    view_pad_right = pad_right;
}

static void vkb_wide_configure(int wide_w, int offset) {
    if (!s_ready) return;
    if (wide_w == s_wide_w && offset == s_wide_offset) return;
    flush_tex_batch(); flush_geometry();
    if (wide_w <= 0) { wide_free_all(); s_wide_w = 0; s_wide_offset = 0; return; }
    if (wide_w != s_wide_w) wide_free_all();
    s_wide_w = wide_w;
    s_wide_offset = offset;
}

/* Select the wide surface to mirror into for the back buffer at base_x. */
static void vkb_wide_set_target(int base_x) {
    if (!s_ready) { s_wide_cur = -1; return; }
    if (s_wide_cur >= 0 && s_wide_base[s_wide_cur] == base_x) { s_wide_cur_base = base_x; return; }
    flush_tex_batch(); flush_geometry();   /* drain into the OLD target first */
    s_wide_cur = wide_surf_for(base_x);
    s_wide_cur_base = base_x;
}

/* Stop mirroring (offscreen draws that don't target a framebuffer). */
static void vkb_wide_disable_target(void) {
    if (s_wide_cur < 0) return;
    flush_tex_batch(); flush_geometry();
    s_wide_cur = -1;
}

/* Mirror a framebuffer clear: fill the full wide width over [y, y+h) of the
 * surface for base_x (revealed margins stay clean). Stencil clears to bit15,
 * like the canonical fill path. */
static void vkb_wide_clear(int base_x, int y, int h, uint16_t color) {
    if (!s_ready || s_wide_w <= 0) return;
    flush_tex_batch(); flush_geometry();
    int i = wide_surf_for(base_x);
    if (i < 0) return;
    s_perf_cur.wide_clears++;
    int S = s_scale, H = VRAM_H * S;
    int y0 = y * S, y1 = (y + h) * S;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0) return;
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    color_self_barrier(cb, s_wide_img[i]);
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = s_rpass; rp.framebuffer = s_wide_fb[i];
    rp.renderArea.extent.width = (uint32_t)(s_wide_w * S);
    rp.renderArea.extent.height = (uint32_t)H;
    p_vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca[2] = {0};
    ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca[0].colorAttachment = 0;
    ca[0].clearValue.color.float32[0] = (float)((color)       & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[1] = (float)((color >> 5)  & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[2] = (float)((color >> 10) & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[3] = (color >> 15) & 1 ? 1.0f : 0.0f;
    ca[1].aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    ca[1].clearValue.depthStencil.depth = 0.0f;
    ca[1].clearValue.depthStencil.stencil = (color >> 15) & 1;
    VkClearRect cr = { { { 0, y0 }, { (uint32_t)(s_wide_w * S), (uint32_t)(y1 - y0) } }, 0, 1 };
    p_vkCmdClearAttachments(cb, 2, ca, 1, &cr);
    p_vkCmdEndRenderPass(cb);
    end_oneshot(cb);
}

/* Clear only the two synthetic reveal strips, leaving the guest-owned centre
 * intact. Mirrors the software/OpenGL opt-in transition cleanup. */
static void vkb_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) {
    if (!s_ready || s_wide_w <= 0 || s_wide_offset <= 0) return;
    flush_tex_batch(); flush_geometry();
    int i = wide_surf_for(base_x);
    if (i < 0) return;
    s_perf_cur.wide_clears++;
    int S = s_scale, H = VRAM_H * S;
    int W = s_wide_w * S, margin = s_wide_offset * S;
    int y0 = y * S, y1 = (y + h) * S;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0 || margin * 2 >= W) return;
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    color_self_barrier(cb, s_wide_img[i]);
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = s_rpass; rp.framebuffer = s_wide_fb[i];
    rp.renderArea.extent.width = (uint32_t)W;
    rp.renderArea.extent.height = (uint32_t)H;
    p_vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca[2] = {0};
    ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca[0].colorAttachment = 0;
    ca[0].clearValue.color.float32[0] = (float)((color)       & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[1] = (float)((color >> 5)  & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[2] = (float)((color >> 10) & 0x1F) / 31.0f;
    ca[0].clearValue.color.float32[3] = (color >> 15) & 1 ? 1.0f : 0.0f;
    ca[1].aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    ca[1].clearValue.depthStencil.depth = 0.0f;
    ca[1].clearValue.depthStencil.stencil = (color >> 15) & 1;
    VkClearRect cr[2];
    uint32_t ncr = 0;
    if (sides & 1)
        cr[ncr++] = (VkClearRect){ { { 0, y0 }, { (uint32_t)margin, (uint32_t)(y1 - y0) } }, 0, 1 };
    if (sides & 2)
        cr[ncr++] = (VkClearRect){ { { W - margin, y0 }, { (uint32_t)margin, (uint32_t)(y1 - y0) } }, 0, 1 };
    if (ncr) p_vkCmdClearAttachments(cb, 2, ca, ncr, cr);
    p_vkCmdEndRenderPass(cb);
    end_oneshot(cb);
}

/* Present source (CPU): read the wide surface band for the displayed buffer
 * into an ARGB8888 buffer, byte-compatible with sw_render_wide_display — used
 * by the shared CPU present fallback and the ws debug dump. The GPU-direct
 * vk_renderer_present_wide is the per-frame path; this readback is cold. */
static int vkb_render_wide_display(uint32_t *out, int pitch, int base_x,
                                   int disp_y, int disp_h) {
    if (!s_ready || s_wide_w <= 0) return 0;
    int i = -1;
    for (int k = 0; k < VK_WIDE_MAX_SURF; k++)
        if (s_wide_img[k] && s_wide_base[k] == base_x) { i = k; break; }
    if (i < 0) return 0;
    flush_cpu_upload(); flush_tex_batch(); flush_geometry();
    int S = s_scale, W = s_wide_w * S, H = VRAM_H * S;
    int out_h = disp_h * S, ry0 = disp_y * S;
    if (ry0 < 0) ry0 = 0;
    if (ry0 + out_h > H) out_h = H - ry0;
    if (out_h <= 0) return 0;
    VkBuffer buf; VkDeviceMemory mem; void *map;
    if (!make_staging((VkDeviceSize)W * out_h * 4, &buf, &mem, &map)) return 0;
    VkCommandBuffer cb = begin_oneshot();
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rc = {0};
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rc.imageSubresource.layerCount = 1;
    rc.imageOffset.y = ry0;
    rc.imageExtent.width = (uint32_t)W; rc.imageExtent.height = (uint32_t)out_h;
    rc.imageExtent.depth = 1;
    p_vkCmdCopyImageToBuffer(cb, s_wide_img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rc);
    img_to(cb, s_wide_img[i], &s_wide_layout[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    end_oneshot(cb);
    gpu_sync();   /* readback must be complete before the CPU reads it */
    const uint8_t *src = (const uint8_t *)map;
    int count = 0;
    for (int row = 0; row < out_h; row++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)out + (size_t)row * pitch);
        const uint8_t *s8 = src + (size_t)row * W * 4;
        for (int col = 0; col < W; col++) {   /* RGBA8 -> 0xAARRGGBB, alpha forced */
            dst[col] = 0xFF000000u | ((uint32_t)s8[col * 4 + 0] << 16)
                     | ((uint32_t)s8[col * 4 + 1] << 8) | s8[col * 4 + 2];
            count++;
        }
    }
    free_staging(buf, mem);
    return count;
}

static const GpuRenderBackend VK_BACKEND = {
    .name                          = "vulkan",
    .init                          = vkb_init,
    .set_scale                     = vkb_set_scale,
    .scale                         = vkb_scale,
    .set_texture_filter            = vkb_set_texture_filter,
    .texture_filter                = vkb_texture_filter,
    .set_semi_transparency         = vkb_set_semi_transparency,
    .set_mask_bits                 = vkb_set_mask_bits,
    .set_texture_window            = vkb_set_texture_window,
    .set_color_modulation          = vkb_set_color_modulation,
    .set_precise_triangle          = vkb_set_precise_triangle,
    .set_perspective_triangle      = vkb_set_perspective_triangle,
    .fill_rect                     = vkb_fill_rect,
    .copy_rect                     = vkb_copy_rect,
    .draw_flat_triangle            = vkb_draw_flat_triangle,
    .draw_gouraud_triangle         = vkb_draw_gouraud_triangle,
    .draw_textured_triangle        = vkb_draw_textured_triangle,
    .draw_shaded_textured_triangle = vkb_draw_shaded_textured_triangle,
    .draw_flat_rect                = vkb_draw_flat_rect,
    .draw_textured_rect            = vkb_draw_textured_rect,
    .draw_textured_rect_scaled     = vkb_draw_textured_rect_scaled,
    .draw_line                     = vkb_draw_line,
    .draw_shaded_line              = vkb_draw_shaded_line,
    .render_display                = vkb_render_display,
    .render_display_hires          = vkb_render_display_hires,
    .vram_write                    = vkb_vram_write,
    .vram_read                     = vkb_vram_read,
    .vram_read_display             = vkb_vram_read_display,
    .vram_transfer_in              = vkb_vram_transfer_in,
    .vram_transfer_out             = vkb_vram_transfer_out,
    .set_draw_area                 = vkb_set_draw_area,
    .get_draw_area                 = vkb_get_draw_area,
    .set_draw_offset               = vkb_set_draw_offset,
    .wide_configure                = vkb_wide_configure,
    .wide_set_view = vkb_wide_set_view,
    .wide_set_target               = vkb_wide_set_target,
    .wide_disable_target           = vkb_wide_disable_target,
    .wide_clear                    = vkb_wide_clear,
    .wide_clear_margins            = vkb_wide_clear_margins,
    .render_wide_display           = vkb_render_wide_display,
};

/* Returned unconditionally (like gl_backend_get): the table is selected before
 * the context exists; vk_renderer_init_context creates the device, and on
 * failure main.cpp switches the facade back to software. */
const GpuRenderBackend *vk_backend_get(void) { return &VK_BACKEND; }

#endif /* PSX_HAVE_VULKAN */
