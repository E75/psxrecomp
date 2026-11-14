#version 450
/* Light shafts ([video] volumetrics = "lightshafts" | "both",
 * docs/EFFECTS.md).
 *
 * Screen-space pass over the COMPOSED presented image (never guest VRAM): a
 * bright-source mask (sky / stand highlights) blurred radially toward the
 * projected light position and added back. This is the classic cheap god-ray
 * approximation - a directional blur of the brightest pixels, not a scattering
 * integral - so it cannot darken anything and costs one fetch per tap.
 *
 * Taps: u_params.y (even, 2..64). Light position: u_params.zw in picture space.
 * The pass reads the intermediate post image the present draw rendered into,
 * so the letterbox rect maps picture space onto that image. */
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 0, binding = 0) uniform sampler2D u_src;   /* composed picture */
layout(push_constant) uniform PC {
    vec4 u_src_rect;    /* picture rect in the post image: x0,y0,x1,y1 (normalized) */
    vec4 u_params;      /* x = strength, y = taps, z = light x, w = light y        */
} pc;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 sample_picture(vec2 p) {
    return texture(u_src, mix(pc.u_src_rect.xy, pc.u_src_rect.zw, p)).rgb;
}

void main() {
    vec2 p = v_uv;                              /* picture space [0,1] */
    vec3 base = sample_picture(p);
    int taps = int(pc.u_params.y + 0.5);
    vec2 light = pc.u_params.zw;
    vec2 delta = (p - light) / float(taps);
    vec3 acc = vec3(0.0);
    float decay = 1.0;
    for (int i = 0; i < taps; i++) {
        vec3 c = sample_picture(p - delta * float(i));
        /* Bright-source mask: only highlights cast shafts. */
        float m = smoothstep(0.62, 0.92, luma(c));
        acc += c * m * decay;
        decay *= 0.965;
    }
    acc *= 1.0 / float(taps);
    /* Falloff from the light so the blur reads as a shaft, not a full-screen
     * haze; x is stretched slightly to follow the picture's 4:3 pixel aspect. */
    float dist = length((p - light) * vec2(1.0, 1.6));
    float falloff = clamp(1.0 - dist * 0.9, 0.0, 1.0);
    o_col = vec4(base + acc * pc.u_params.x * falloff, 1.0);
}
