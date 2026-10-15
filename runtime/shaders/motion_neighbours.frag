#version 450
// Motion blur (post.c): each tile's strongest motion among it and its neighbours - what can smear into it
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

void main() {
    ivec2 t = ivec2(gl_FragCoord.xy), last = textureSize(source, 0) - 1;
    vec2 best = vec2(0.0);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec2 m = texelFetch(source, clamp(t + ivec2(x, y), ivec2(0), last), 0).xy;
            if (dot(m, m) > dot(best, best))
                best = m;
        }
    out_color = vec4(best, 0.0, 1.0);
}
