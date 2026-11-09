#version 450
// Ambient occlusion (render_ao.c) on the picture: multiplied in (the blending), from the half resolution occlusion - of
// its 4 nearest, those at this pixel's distance, so that a thing's occlusion doesn't spill onto what is behind it.
// Again for a later part of the world: only where that drew (the depth changed).
#include "ao_common.glsl"
layout(location = 0) out vec4 out_color;

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy), last = ivec2(pc.area.zw) - 1;
    if (is_water(pix) || (pc.params2.w > 0.5 && texelFetch(depth, pix, 0).r == texelFetch(done, pix, 0).r)) {
        out_color = vec4(1.0);
        return;
    }
    vec3 p = view_pos(pix);
    float dist = length(p);
    vec2 h = (gl_FragCoord.xy - 0.5) / 2.0 - 0.25;  // texel i stands for full-resolution pixel 2i
    ivec2 base = ivec2(floor(h));
    vec2 f = h - vec2(base);
    float sum = 0.0, weight = 0.0;
    for (int j = 0; j < 4; j++) {
        ivec2 o = ivec2(j & 1, j >> 1);
        vec2 s = texelFetch(source, clamp(base + o, ivec2(0), last), 0).rg;
        float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) + 1e-3;
        float w = bilinear / (1e-3 + abs(s.g - dist) / max(dist, 1.0) * 40.0);
        sum += s.r * w;
        weight += w;
    }
    // where the point lights shine (as ge.frag lights by them), less: the occlusion takes away the light from all
    // round, not theirs - as much less as theirs is of the light there (the scenery's: its colour times 1 + theirs)
    float local = 0.0;
    for (int i = 0; i < 4; i++) {
        if (pc.lights[i].w <= 0.0)
            continue;
        float r = clamp(length(pc.lights[i].xyz - p) / pc.lights[i].w, 0.0, 1.0);
        vec3 c = pc.colors[i].rgb;
        local += (1.0 - r * r) * (1.0 - r * r) * max(c.r, max(c.g, c.b));
    }
    out_color = vec4(vec3(mix(sum / weight, 1.0, local / (1.0 + local))), 1.0);
}
