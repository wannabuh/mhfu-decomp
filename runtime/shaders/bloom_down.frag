#version 450
// Bloom (post.c), down a level: 13 taps (Jimenez, "Next generation post processing in Call of Duty"). The first pass,
// from the picture, takes the light the effects added (the glow: fire, glows) and the picture's own light above a
// threshold (soft knee), its taps weighted by their brightness (Karis) so that single bright pixels don't flicker.
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

vec3 above(vec3 c) {
    float m = max(c.r, max(c.g, c.b)), e = m - pc.bloom.y, knee = 0.1;
    float excess = e <= 0.0 ? 0.0 : e < knee ? e * e / (2.0 * knee) : e - 0.5 * knee;
    return c * (excess / max(m, 1e-5));
}

vec3 tap(vec2 uv) {
    if (pc.texel.z > 0.5)
        return above(texture(source, uv).rgb) * pc.bloom.z + texture(source2, uv).rgb * pc.bloom.w;
    return texture(source, uv).rgb;
}

float weight(vec3 c) {
    return 1.0 / (1.0 + max(c.r, max(c.g, c.b)));
}

void main() {
    vec2 ts = pc.texel.xy, uv = gl_FragCoord.xy * 2.0 * ts;
    vec3 a = tap(uv + ts * vec2(-2, -2)), b = tap(uv + ts * vec2(0, -2)), c = tap(uv + ts * vec2(2, -2));
    vec3 d = tap(uv + ts * vec2(-2, 0)), e = tap(uv), f = tap(uv + ts * vec2(2, 0));
    vec3 g = tap(uv + ts * vec2(-2, 2)), h = tap(uv + ts * vec2(0, 2)), i = tap(uv + ts * vec2(2, 2));
    vec3 j = tap(uv + ts * vec2(-1, -1)), k = tap(uv + ts * vec2(1, -1));
    vec3 l = tap(uv + ts * vec2(-1, 1)), m = tap(uv + ts * vec2(1, 1));
    vec3 r;
    if (pc.texel.z > 0.5) {             // five 2 x 2 boxes, by their brightness
        vec3 b0 = (j + k + l + m) * 0.25, b1 = (a + b + d + e) * 0.25, b2 = (b + c + e + f) * 0.25;
        vec3 b3 = (d + e + g + h) * 0.25, b4 = (e + f + h + i) * 0.25;
        float w0 = weight(b0) * 0.5, w1 = weight(b1) * 0.125, w2 = weight(b2) * 0.125, w3 = weight(b3) * 0.125;
        float w4 = weight(b4) * 0.125;
        r = (b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3 + b4 * w4) / (w0 + w1 + w2 + w3 + w4);
    } else {
        r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    out_color = vec4(max(r, vec3(0.0)), 1.0);
}
