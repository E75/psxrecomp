#ifndef PSX_VFX_H
#define PSX_VFX_H

/* vfx.h - higher-level rendering effects config (docs/EFFECTS.md).
 *
 * Two effects, both OFF by default and both Vulkan-only:
 *   fog      per-primitive distance fade toward fog_color, applied in the
 *            PRESENT pass from a per-pixel depth proxy the geometry pass
 *            writes into the (otherwise unused) depth attachment.
 *   shafts   screen-space radial blur ("light shafts") of the composed
 *            presented image toward a projected light position.
 *
 * Nothing here touches guest-visible VRAM: the guest framebuffer contents the
 * game reads back (savestates, screenshots, 24-bit present, the pack mirror)
 * are written by the unchanged geometry path.
 *
 * Config comes from game.toml [video] (volumetrics / fog_density / fog_color /
 * lightshaft_strength / lightshaft_samples) with PSX_VFX* env overrides, read
 * here at runtime so this module stays a plain C unit (same pattern as
 * vram_ext.c). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VFX_FOG    0x1
#define VFX_SHAFTS 0x2

/* Parse the config. Idempotent; safe to call before any renderer exists. */
void  vfx_init(void);

/* Armed mode bitmask (VFX_FOG|VFX_SHAFTS). 0 == off == the pre-effect path. */
int   vfx_mode(void);
int   vfx_fog_enabled(void);
int   vfx_shafts_enabled(void);

float vfx_fog_density(void);            /* 0..1 */
void  vfx_fog_color(float out_rgb[3]);  /* linear 0..1 from "#RRGGBB" */
float vfx_shaft_strength(void);
int   vfx_shaft_samples(void);          /* even, 2..64 */
void  vfx_shaft_light_uv(float out_uv[2]);   /* projected light position */

/* Backend reporting: what actually got armed (the renderer may refuse an
 * effect its device cannot support) and the last measured pass cost. */
void  vfx_set_active(int fog, int shafts);
int   vfx_active_fog(void);
int   vfx_active_shafts(void);
void  vfx_report_cost(float ms);        /* per frame, 0 when no pass ran */
float vfx_last_cost_ms(void);

/* Debug server "vfx_state": one JSON object. Returns bytes written. */
int   vfx_state_json(char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_VFX_H */
