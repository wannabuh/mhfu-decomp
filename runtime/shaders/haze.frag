#version 450
// Post-processing (post.c), the first pass: the haze - before the anti-aliasing and the motion blur, which smear the
// picture away from its depth.
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

// Haze (aerial perspective): the air between the camera and what it sees, thicker the lower it lies - integrated along
// the way to the point (an exponential fall with height, from the eye's) - the colour of the world's fog, lit towards the
// sun by the light it scatters. Overhead the sky stays clear; towards the horizon, far hills and the sky's foot fade into
// it. The game's own fog is on the scenery already: this is the air it has none of
const float HAZE_DENSITY = 1.0 / 26000.0;  // per world unit, at the eye's height
const float HAZE_FALL = 1.0 / 1800.0;      // per world unit up
const float HAZE_START = 2000.0;           // world units: none nearer
vec3 hazed(vec3 c, ivec2 pix) {
    uint bits = floatBitsToUint(pc.haze.w);
    float strength = float(bits >> 24) / 64.0;
    vec3 up = pc.haze.xyz;
    if (strength <= 0.0 || dot(up, up) < 0.5)
        return c;
    float d = texelFetch(depth, pix, 0).r;
    // (nothing drawn: as far as the farthest the camera sees)
    vec3 p = d > 0.0 ? view_pos(pix, d) : normalize(view_pos(pix, 1e-6)) * 60000.0;
    float dist = min(length(p), 60000.0);
    vec3 dir = normalize(p);
    float h = dot(dir, up) * dist;                  // how far above the eye it is
    float a = clamp(HAZE_FALL * h, -2.5, 60.0);
    float along = abs(a) < 1e-3 ? dist : dist * (1.0 - exp(-a)) / a;
    // (none this side of HAZE_START: rooms and caves stay clear)
    float amount = 1.0 - exp(-strength * HAZE_DENSITY * max(along - HAZE_START, 0.0));
    vec3 fog = vec3(float(bits & 255u), float((bits >> 8) & 255u), float((bits >> 16) & 255u)) / 255.0;
    vec3 air = mix(fog, vec3(luma(fog)) * vec3(0.92, 0.97, 1.08), 0.3);
    float towards = max(dot(dir, pc.sun.xyz), 0.0);
    air += pc.sun_color.rgb * (pow(towards, 8.0) * 0.35 + pow(towards, 64.0) * 0.25);
    return mix(c, min(air, vec3(1.0)), amount * 0.85);
}

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec4 s = texelFetch(source, pix, 0);
    out_color = vec4(hazed(s.rgb, pix), s.a);
}
