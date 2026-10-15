#version 450
// Depth of field (post.c): the blur, at half resolution - samples on a disc (a golden-angle spiral) as large as this
// pixel's blur. A sample counts within the smaller of its own blur and this pixel's: what is sharper (nearer) doesn't
// spread over what is behind it.
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), last = textureSize(source, 0) - 1;
    vec4 centre = texelFetch(source, p, 0);
    float radius = centre.a;
    if (radius < 0.5) {
        out_color = centre;
        return;
    }
    const int SAMPLES = 40;
    vec3 sum = centre.rgb;
    float weight = 1.0;
    for (int i = 0; i < SAMPLES; i++) {
        float r = radius * sqrt((float(i) + 0.5) / float(SAMPLES)), angle = float(i) * 2.3999632;
        vec4 s = texelFetch(source, clamp(p + ivec2(round(vec2(cos(angle), sin(angle)) * r)), ivec2(0), last), 0);
        float cover = clamp(min(s.a, radius) - r + 0.5, 0.0, 1.0);
        float sigma = max(0.5 * radius, 0.5);
        float w = cover * exp(-r * r / (2.0 * sigma * sigma));
        sum += s.rgb * w;
        weight += w;
    }
    out_color = vec4(sum / weight, radius);
}
