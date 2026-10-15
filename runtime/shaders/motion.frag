#version 450
// Motion blur (post.c), the first pass: each pixel smeared along the way it moved on the screen since the last frame
// (where it was then: its place in the world - from the depth - through the last frame's camera; the sky, a
// direction, by the camera's turning alone), as far as the shutter stays open. The hunter, monsters and people (the
// glow's alpha: render.c draws them into it) stay sharp, and lend nothing to what is smeared past them.
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

const int SAMPLES = 12;

// The screen motion (render target pixels) of a pixel, as far as the shutter takes it
vec2 motion(ivec2 pix) {
    float d = texelFetch(depth, pix, 0).r;
    vec3 p = view_pos(pix, d > 0.0 ? d : 1.0);
    vec4 q = d > 0.0 ? pc.to_shadow * vec4(p, 1.0) : pc.to_shadow * vec4(normalize(p), 0.0);
    if (q.w <= 1e-6)
        return vec2(0.0);
    vec2 now = (vec2(pix) + 0.5) / pc.area.z, then = q.xy / q.w;
    vec2 m = (now - then) * pc.area.z * pc.texel.x;
    float len = length(m);
    return len > pc.texel.y ? m * (pc.texel.y / len) : m;
}

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy), last = ivec2(pc.area.xy) - 1;
    vec4 centre = texelFetch(source, pix, 0);
    if (texelFetch(source2, pix, 0).a > 0.5) {
        out_color = centre;
        return;
    }
    vec2 m = motion(pix);
    if (length(m) < 0.75) {
        out_color = centre;
        return;
    }
    // samples along it, centred (the moment shown in the middle of the exposure), from a place differing per pixel
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
    vec3 sum = centre.rgb;
    float weight = 1.0;
    for (int i = 0; i < SAMPLES; i++) {
        float t = (float(i) + 0.5 + noise) / float(SAMPLES) - 0.5;
        ivec2 q = clamp(ivec2(vec2(pix) + 0.5 + m * t), ivec2(0), last);
        if (texelFetch(source2, q, 0).a > 0.5)
            continue;
        sum += texelFetch(source, q, 0).rgb;
        weight += 1.0;
    }
    out_color = vec4(sum / weight, centre.a);
}
