#version 450
// Motion blur (post.c): the strongest motion in each tile of the picture (sampled every few pixels)
#include "motion_common.glsl"
layout(location = 0) out vec4 out_color;

void main() {
    int size = int(pc.texel.z), stride = max(size / 8, 1);
    ivec2 base = ivec2(gl_FragCoord.xy) * size, last = ivec2(pc.area.xy) - 1;
    vec2 best = vec2(0.0);
    for (int y = stride / 2; y < size; y += stride)
        for (int x = stride / 2; x < size; x += stride) {
            vec2 m = motion(min(base + ivec2(x, y), last));
            if (dot(m, m) > dot(best, best))
                best = m;
        }
    out_color = vec4(best, 0.0, 1.0);
}
