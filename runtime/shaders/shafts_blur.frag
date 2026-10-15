#version 450
// Light shafts (post.c): blurred one way at half resolution - the neighbours mixed in unless they are at a very
// different distance (a near thing's edge against what is far behind it).
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

void main() {
    ivec2 c = ivec2(gl_FragCoord.xy), last = textureSize(source, 0) - 1;
    vec4 centre = texelFetch(source, c, 0);
    vec3 sum = centre.rgb;
    float weight = 1.0;
    for (int i = -4; i <= 4; i++) {
        if (i == 0)
            continue;
        vec4 s = texelFetch(source, clamp(c + ivec2(pc.texel.xy) * i, ivec2(0), last), 0);
        float w = exp(-float(i * i) / 8.0) * exp(-abs(s.a - centre.a) * 2.0 / max(min(s.a, centre.a), 1.0));
        sum += s.rgb * w;
        weight += w;
    }
    out_color = vec4(sum / weight, centre.a);
}
