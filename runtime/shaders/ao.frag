#version 450
// Ambient occlusion (render.c), at half resolution from the world's depth: how much of the hemisphere above each point
// the things near it cover (horizon-based, normals from the depth). Out: the light left (1 open) and the distance, for
// the blur.
#include "ao_common.glsl"
layout(location = 0) out vec4 out_ao;

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy) * 2;     // the full-resolution pixel this one stands for
    ivec2 last = ivec2(pc.area.xy) - 1;
    vec3 p = view_pos(pix);
    float dist = length(p);
    if (!(dist < pc.params2.x)) {               // the sky, or too far to matter
        out_ao = vec4(1.0, 1e6, 0.0, 0.0);
        return;
    }
    // the normal from the neighbours on the side with the smaller step in depth (no smearing across edges)
    vec3 r = view_pos(min(pix + ivec2(1, 0), last)) - p, l = p - view_pos(max(pix - ivec2(1, 0), ivec2(0)));
    vec3 d = view_pos(min(pix + ivec2(0, 1), last)) - p, u = p - view_pos(max(pix - ivec2(0, 1), ivec2(0)));
    vec3 dx = dot(r, r) < dot(l, l) ? r : l, dy = dot(d, d) < dot(u, u) ? d : u;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0)                        // facing the camera (at the origin)
        n = -n;

    float radius = pc.params.x;
    float pix_radius = clamp(radius * pc.params.w / max(dist, 1.0), 2.0, 96.0 * pc.params.z / 6.0);
    // each pixel's own rotation and steps (interleaved gradient noise); the blur takes the pattern out
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    const int DIRS = 6, STEPS = 4;
    float occlusion = 0.0;
    for (int i = 0; i < DIRS; i++) {
        float a = (float(i) + noise) * (6.2831853 / float(DIRS));
        vec2 dir = vec2(cos(a), sin(a));
        for (int s = 0; s < STEPS; s++) {
            float t = (float(s) + fract(noise * 7.0 + float(s) * 0.618)) / float(STEPS);
            ivec2 q = clamp(ivec2(vec2(pix) + dir * (t * pix_radius + 1.0)), ivec2(0), last);
            vec3 v = view_pos(q) - p;
            float len2 = dot(v, v);
            if (len2 < 1e-6)
                continue;
            float cosine = dot(n, v) * inversesqrt(len2);
            occlusion += clamp(cosine - 0.15, 0.0, 1.0) * clamp(1.0 - len2 / (radius * radius), 0.0, 1.0);
        }
    }
    float ao = clamp(1.0 - 2.0 * pc.params.y * occlusion / float(DIRS * STEPS), 0.0, 1.0);
    ao = mix(ao, 1.0, smoothstep(0.5 * pc.params2.x, pc.params2.x, dist));
    out_ao = vec4(ao, dist, 0.0, 0.0);
}
