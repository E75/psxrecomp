#version 450
/* Fog present ([video] volumetrics = "fog" | "both", docs/EFFECTS.md).
 *
 * Same pass as present.frag - sample the displayed VRAM rect and write it
 * OPAQUE - plus a distance fade toward the fog colour. The per-pixel factor
 * comes from u_depth: the depth attachment the geometry pass used as a
 * distance-proxy store (PGXP normalized 1/z, else the primitive's ordering-
 * table rank). The guest framebuffer itself is never modified: this is the
 * last stage before the swapchain. */
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 0, binding = 0) uniform sampler2D u_src;    /* displayed VRAM rect */
layout(set = 0, binding = 1) uniform sampler2D u_depth;  /* fog depth proxy    */
layout(push_constant) uniform PC {
    vec4 u_src_rect;    /* displayed region in normalized src coords: x0,y0,x1,y1 */
    vec4 u_fog;         /* rgb = fog colour, a = density (0..1)                    */
} pc;
void main() {
    vec2 uv = mix(pc.u_src_rect.xy, pc.u_src_rect.zw, v_uv);
    vec3 c = texture(u_src, uv).rgb;
    /* Depth is cleared to 0 (no fog) and written per primitive, so sky and
     * anything the 3D pass did not cover keep the guest's exact colour. */
    float d = clamp(texture(u_depth, uv).r, 0.0, 1.0);
    o_col = vec4(mix(c, pc.u_fog.rgb, d * pc.u_fog.a), 1.0);
}
