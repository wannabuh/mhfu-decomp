#version 450
// Ambient occlusion (render_ao.c): blurred one way at half resolution, as much as the neighbours are at the same distance
// (another thing's doesn't mix in).
#include "ao_common.glsl"
layout(location = 0) out vec4 out_ao;

void main() {
    ivec2 c = ivec2(gl_FragCoord.xy), last = ivec2(pc.area.zw) - 1;
    vec2 centre = texelFetch(source, c, 0).rg;
    float sum = centre.r, weight = 1.0;
    for (int i = -4; i <= 4; i++) {
        if (i == 0)
            continue;
        vec2 s = texelFetch(source, clamp(c + ivec2(pc.params2.yz) * i, ivec2(0), last), 0).rg;
        float w = exp(-float(i * i) / 8.0) * exp(-abs(s.g - centre.g) * 8.0 / max(centre.g, 1e-3));
        sum += s.r * w;
        weight += w;
    }
    out_ao = vec4(sum / weight, centre.g, 0.0, 0.0);
}
