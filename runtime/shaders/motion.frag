#version 450
// Motion blur (post.c), the last of its passes: McGuire et al.'s reconstruction filter ("A Reconstruction Filter for
// Plausible Motion Blur", 2012), as ao-vk's. Samples along the neighbourhood's strongest motion; each counts if it is
// behind this pixel and this pixel's own motion covers it, or in front and its motion covers this pixel - so what
// moves smears over what stands behind it and not the reverse, and what the camera follows stays sharp.
#include "motion_common.glsl"
layout(location = 0) out vec4 out_color;

const int SAMPLES = 15;

float cone(float dist, float radius) { return clamp(1.0 - dist / max(radius, 1e-3), 0.0, 1.0); }
float cylinder(float dist, float radius) { return 1.0 - smoothstep(0.95 * radius, 1.05 * radius + 1e-3, dist); }
float soft_depth(float za, float zb) { return clamp(1.0 - (za - zb) / max(0.02 * za, 0.05), 0.0, 1.0); }   // a in front

float dist_at(ivec2 pix) {
    float d = texelFetch(depth, pix, 0).r;
    return d > 0.0 ? length(view_pos(pix, d)) : 1e6;
}

void main() {
    ivec2 x = ivec2(gl_FragCoord.xy), last = ivec2(pc.area.xy) - 1;
    vec4 centre = texelFetch(source, x, 0);
    vec2 vn = texelFetch(source3, x / int(pc.texel.z), 0).xy;
    float rn = 0.5 * length(vn);
    if (rn < 0.5) {
        out_color = centre;
        return;
    }
    vec2 vx = motion(x);
    float rx = max(0.5 * length(vx), 0.5), zx = dist_at(x);
    float weight = 1.0 / rx;
    vec3 sum = centre.rgb * weight;
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
    for (int i = 0; i < SAMPLES; i++) {
        if (i == SAMPLES / 2)
            continue;
        float t = mix(-1.0, 1.0, (float(i) + jitter + 1.0) / float(SAMPLES + 1));
        ivec2 y = clamp(ivec2(vec2(x) + 0.5 + vn * 0.5 * t), ivec2(0), last);
        float zy = dist_at(y);
        float ry = max(0.5 * length(motion(y)), 0.5), dist = length(vec2(y - x));
        float background = soft_depth(zx, zy), foreground = soft_depth(zy, zx);
        float a = foreground * cone(dist, ry) + background * cone(dist, rx) + cylinder(dist, ry) * cylinder(dist, rx) * 2.0;
        sum += texelFetch(source, y, 0).rgb * a;
        weight += a;
    }
    out_color = vec4(sum / weight, centre.a);
}
