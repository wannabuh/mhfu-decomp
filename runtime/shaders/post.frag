#version 450
// Post-processing (post.c), the last pass: the picture with its depth of field, its bloom and light shafts (added the
// less the brighter it is already, so that nothing overshoots white), colour graded, back into the render target.
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

// Warmth (white balance), contrast (in a perceptual square-root space, about its middle), saturation, vignette
vec3 grade(vec3 c) {
    float warmth = pc.grade.z;
    c *= vec3(1.0 + 0.1 * warmth, 1.0 + 0.02 * warmth, 1.0 - 0.1 * warmth);
    vec3 s = sqrt(max(c, vec3(0.0)));
    s = max((s - 0.5) * pc.grade.y + 0.5, vec3(0.0));
    c = s * s;
    c = mix(vec3(luma(c)), c, pc.grade.x);
    if (pc.grade.w > 0.0) {
        vec2 uv = gl_FragCoord.xy / pc.area.xy - 0.5;
        c *= 1.0 - pc.grade.w * smoothstep(0.35, 0.9, length(uv * vec2(1.0, 0.75)) * 1.4);
    }
    return clamp(c, 0.0, 1.0);
}

void main() {
    vec4 s = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
    vec2 uv = gl_FragCoord.xy / pc.area.xy;
    vec3 c = s.rgb;
    if (pc.dof.z > 0.0) {               // the depth of field's blur, as much as this point is blurred
        ivec2 pix = ivec2(gl_FragCoord.xy);
        float d = texelFetch(depth, pix, 0).r;
        float k = clamp(coc(d > 0.0 ? length(view_pos(pix, d)) : 1e6) / 1.5, 0.0, 1.0);
        c = mix(c, texture(source4, uv).rgb, k);
    }
    if (pc.bloom.x > 0.0) {
        vec3 b = texture(source2, uv).rgb * pc.bloom.x;
        b /= max(1.0, max(b.r, max(b.g, b.b)));     // (its hue kept)
        float k = 1.0 - luma(c);
        c = min(c + b * k * k, vec3(1.0));  // (on what is dark: the flames themselves keep their colour)
    }
    if (pc.sun.w > 0.0) {
        vec3 v = min(texture(source3, uv).rgb, vec3(1.0));
        c = 1.0 - (1.0 - c) * (1.0 - v);
    }
    out_color = vec4(grade(c), s.a);
    if (pc.area.w == 1.0)               // (MHFU_POST_DEBUG: the bloom, the light shafts, by themselves)
        out_color = vec4(texture(source2, uv).rgb * pc.bloom.x, 1.0);
    else if (pc.area.w == 2.0)
        out_color = vec4(texture(source3, uv).rgb * 4.0, 1.0);
}
