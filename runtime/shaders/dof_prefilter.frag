#version 450
// Depth of field (post.c), first: the picture at half resolution, with how blurred each pixel is (alpha) - the
// nearest of its four's, so that a sharp thing's outline isn't blurred away.
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * 2, last = ivec2(pc.area.xy) - 1;
    vec3 sum = vec3(0.0);
    float nearest = 1e6;
    for (int i = 0; i < 4; i++) {
        ivec2 p = min(base + ivec2(i & 1, i >> 1), last);
        sum += texelFetch(source, p, 0).rgb;
        float d = texelFetch(depth, p, 0).r;
        nearest = min(nearest, d > 0.0 ? length(view_pos(p, d)) : 1e6);
    }
    out_color = vec4(sum * 0.25, coc(nearest));
}
