#version 450
/* Temporal blend for the 60 fps presenter (the Vulkan twin of the GL
 * INTERP_FS): prev/curr are the two most recent displayed frames; alpha is the
 * schedule's position from prev (0) to curr (1). u_blend_mode 1 is the
 * change-adaptive fade: where the two frames differ, hold the nearest one
 * instead of showing a ghost of both. Output is opaque (the AMD compositor
 * honours alpha in the swapchain; see present.frag). */
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 0, binding = 0) uniform sampler2D u_prev;
layout(set = 0, binding = 1) uniform sampler2D u_curr;
layout(push_constant) uniform PC {
    float u_alpha;
    int   u_blend_mode;
} pc;
void main() {
    vec3 prev = texture(u_prev, v_uv).rgb;
    vec3 curr = texture(u_curr, v_uv).rgb;
    float alpha = pc.u_alpha;
    if (pc.u_blend_mode == 1) {
        vec3 d = abs(prev - curr);
        float change = max(max(d.r, d.g), d.b);
        float safe_blend = 1.0 - smoothstep(0.08, 0.20, change);
        alpha = mix(step(0.5, alpha), alpha, safe_blend);
    }
    o_col = vec4(mix(prev, curr, alpha), 1.0);
}
