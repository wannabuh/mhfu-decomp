#version 450
// Light shafts (post.c), at half resolution: the sunlight the air between the camera and each point scatters towards
// the camera, marched through the sun's shadow map - shadowed air stays dark, so light through gaps (between trees,
// past rocks) forms shafts - brightest looking towards the sun (forward scattering). Out: the light, and the distance
// (for the blur).
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

const float REACH = 1500.0;     // how far along the ray the air counts (world units: about the shadow map's reach)
const int STEPS = 24;

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy) * 2;
    float d = texelFetch(depth, pix, 0).r;
    vec3 p = view_pos(pix, d > 0.0 ? d : 1.0);
    float dist = d > 0.0 ? length(p) : 1e6;
    vec3 dir = normalize(p);
    float len = min(dist, REACH);
    // the ray in the shadow map: linear in t
    vec3 s0 = (pc.to_shadow * vec4(0.0, 0.0, 0.0, 1.0)).xyz, sd = (pc.to_shadow * vec4(dir, 0.0)).xyz;
    // steps closer together near the camera (t = len u^2, each weighted by its length), from a place that differs
    // from pixel to pixel (the blur averages them)
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float lit = 0.0, open = 0.0;
    for (int i = 0; i < STEPS; i++) {
        float u = (float(i) + noise) / float(STEPS), t = len * u * u;
        vec3 q = s0 + sd * t;
        float w = 2.0 * u * exp(-t / 2000.0);
        float vis = 1.0;
        if (max(abs(q.x), abs(q.y)) < 1.0 && q.z > 0.0 && q.z < 1.0)
            vis = texture(shadow_map, vec3(q.xy * 0.5 + 0.5, q.z - 0.001));
        lit += vis * w;
        open += w;
    }
    // shafts are contrast: a ray partly in shadow is darkened by a power of its lit share, one lit all along keeps its
    // light (a faint haze)
    float share = open > 0.0 ? clamp(lit / open, 0.0, 1.0) : 0.0;
    lit = open * pow(share, 1.0 + pc.sun_color.w);
    // Henyey-Greenstein (g 0.6: forwards), its peak capped
    const float g = 0.6;
    float cosine = dot(dir, pc.sun.xyz);
    float phase = min((1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosine, 1.5), 4.0);
    vec3 light = pc.sun_color.rgb * (lit * (len / float(STEPS)) * pc.sun.w * phase);
    out_color = vec4(clamp(light, 0.0, 4.0), min(dist, 1e5));
}
