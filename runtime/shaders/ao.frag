#version 450
// Ambient occlusion (render_ao.c), at half resolution from the world's depth: how much of the hemisphere above each point
// the things near it cover (horizon-based, normals from the depth). Out: the light left (1 open) and the distance, for
// the blur.
#include "ao_common.glsl"
layout(location = 0) out vec4 out_ao;

// One way across the surface at a pixel, from its neighbour on the side that goes straight on: a plane's depth is linear
// on the screen, so that side's next pixel lies in line (no smearing across edges, however small the step: the ground's
// pieces meet with a step of about a unit, smaller than the slope from one pixel to the next at a low angle, and the
// side with the smaller difference was the wrong one when the step went back the other way)
vec3 along(ivec2 pix, ivec2 way, vec3 p, ivec2 last) {
    ivec2 a1 = pix + way, a2 = pix + 2 * way, b1 = pix - way, b2 = pix - 2 * way;
    float c = texelFetch(depth, pix, 0).r;
    bool has_a = all(lessThanEqual(a2, last)), has_b = all(greaterThanEqual(b2, ivec2(0)));
    float a = has_a ? abs(2.0 * texelFetch(depth, a1, 0).r - texelFetch(depth, a2, 0).r - c) : 1e9;
    float b = has_b ? abs(2.0 * texelFetch(depth, b1, 0).r - texelFetch(depth, b2, 0).r - c) : 1e9;
    return a < b ? view_pos(a1) - p : p - view_pos(b1);
}

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy) * 2;     // the full-resolution pixel this one stands for
    ivec2 last = ivec2(pc.area.xy) - 1;
    vec3 p = view_pos(pix);
    float dist = length(p);
    if (!(dist < pc.params2.x)) {               // the sky, or too far to matter
        out_ao = vec4(1.0, 1e6, 0.0, 0.0);
        return;
    }
    vec3 dx = along(pix, ivec2(1, 0), p, last), dy = along(pix, ivec2(0, 1), p, last);
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
            // (what rises less than about a seventh of the radius above the surface hides nothing: the water's planes,
            // stacked up to 13 units apart, write depth and their edges are no surfaces: #24. What rises more counts in full)
            float height = dot(n, v);
            float cosine = height * inversesqrt(len2);
            occlusion += clamp(cosine - 0.15, 0.0, 1.0) * clamp(1.0 - len2 / (radius * radius), 0.0, 1.0) *
                         smoothstep(0.12 * radius, 0.17 * radius, height);
        }
    }
    float ao = clamp(1.0 - 2.0 * pc.params.y * occlusion / float(DIRS * STEPS), 0.0, 1.0);
    ao = mix(ao, 1.0, smoothstep(0.5 * pc.params2.x, pc.params2.x, dist));
    out_ao = vec4(ao, dist, 0.0, 0.0);
}
