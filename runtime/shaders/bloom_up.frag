#version 450
// Bloom (post.c), up a level: a 3 x 3 tent over the smaller level, added (the blending) to this level's light.
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

void main() {
    vec2 ts = pc.texel.xy, uv = gl_FragCoord.xy * 0.5 * ts;
    vec3 s = texture(source, uv).rgb * 4.0;
    s += (texture(source, uv + ts * vec2(-1, 0)).rgb + texture(source, uv + ts * vec2(1, 0)).rgb +
          texture(source, uv + ts * vec2(0, -1)).rgb + texture(source, uv + ts * vec2(0, 1)).rgb) * 2.0;
    s += texture(source, uv + ts * vec2(-1, -1)).rgb + texture(source, uv + ts * vec2(1, -1)).rgb +
         texture(source, uv + ts * vec2(-1, 1)).rgb + texture(source, uv + ts * vec2(1, 1)).rgb;
    out_color = vec4(s / 16.0, 1.0);
}
