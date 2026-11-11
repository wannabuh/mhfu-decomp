#include "shared.h"
// Rounder characters (render.c, MHFU_TESSELLATION): each new point curved out by Phong tessellation (Boubekeur and
// Alexa) - the flat triangle's point pulled towards its projections onto the corners' tangent planes (their shape
// normals: the same for every corner at a place, so that the triangles about it curve alike) - and placed on the
// screen by how far it moved (the corners stay where they were).
layout(triangles, fractional_odd_spacing, ccw) in;

layout(std430, set = 0, binding = BIND_LIGHTS) readonly buffer Lights { LightSet sets[]; };

layout(push_constant) uniform Draw { DRAW_PUSH } draw;

layout(location = 0) in vec2 in_uv[];
layout(location = 1) in vec4 in_color[];
layout(location = 2) in float in_fog[];
layout(location = 3) in vec3 in_shadow[];
layout(location = 4) in vec3 in_sun[];
layout(location = 5) in vec3 in_normal[];
layout(location = 6) in vec3 in_world[];
layout(location = 7) in vec4 in_shape[];
layout(location = 8) in vec3 in_motion[];
layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) out float out_fog;
layout(location = 3) out vec3 out_shadow;
layout(location = 4) out vec3 out_sun;
layout(location = 5) out vec3 out_normal;
layout(location = 6) out vec3 out_world;
layout(location = 8) out vec3 out_motion;
#ifdef TL_WOUNDS
// (the GPU's vertex stage's wounds, ge_tl.vert's by ge_tl.tesc: on to the pixels)
layout(location = 10) in vec3 in_wound[][4];
layout(location = 18) in float in_wound_facing[][4];
layout(location = 14) patch in vec4 in_wound_look[4];
layout(location = 22) patch in uint in_wound_tex;
layout(location = 23) patch in vec4 in_wound_run[4];
layout(location = 10) out vec3 out_wound[4];
layout(location = 18) out float out_wound_facing[4];
layout(location = 14) flat out vec4 out_wound_look[4];
layout(location = 22) flat out uint out_wound_tex;
layout(location = 23) flat out vec4 out_wound_run[4];
#endif

const float ROUNDNESS = 0.6;

void main() {
    vec3 b = gl_TessCoord;
    vec3 P0 = in_world[0], P1 = in_world[1], P2 = in_world[2];
    vec3 p = b.x * P0 + b.y * P1 + b.z * P2;
    vec3 n0 = normalize(in_shape[0].xyz), n1 = normalize(in_shape[1].xyz), n2 = normalize(in_shape[2].xyz);
    // Phong's point less the flat one, edge by edge - b_i b_j (pi_i(P_j) + pi_j(P_i) - P_i - P_j), pi_i(q) the
    // projection of q onto corner i's tangent plane - each but those left straight (bit k: the edge facing corner k)
    uint open = uint(in_shape[2].w);
    vec3 e01 = -dot(P1 - P0, n0) * n0 - dot(P0 - P1, n1) * n1;
    vec3 e12 = -dot(P2 - P1, n1) * n1 - dot(P1 - P2, n2) * n2;
    vec3 e20 = -dot(P0 - P2, n2) * n2 - dot(P2 - P0, n0) * n0;
    vec3 bend = ((open & 4u) != 0u ? vec3(0.0) : b.x * b.y * e01) + ((open & 1u) != 0u ? vec3(0.0) : b.y * b.z * e12) +
                ((open & 2u) != 0u ? vec3(0.0) : b.z * b.x * e20);
    vec3 world = p + ROUNDNESS * bend;
    mat4 to_clip = mat4(sets[draw.camera].dir[0], sets[draw.camera].dir[1], sets[draw.camera].dir[2],
                        sets[draw.camera].dir[3]);
    gl_Position = b.x * gl_in[0].gl_Position + b.y * gl_in[1].gl_Position + b.z * gl_in[2].gl_Position +
                  to_clip * vec4(world - p, 0.0);
    out_uv = b.x * in_uv[0] + b.y * in_uv[1] + b.z * in_uv[2];
    out_color = b.x * in_color[0] + b.y * in_color[1] + b.z * in_color[2];
    out_fog = b.x * in_fog[0] + b.y * in_fog[1] + b.z * in_fog[2];
    out_sun = b.x * in_sun[0] + b.y * in_sun[1] + b.z * in_sun[2];
    out_normal = b.x * in_normal[0] + b.y * in_normal[1] + b.z * in_normal[2];
    out_world = world;
    out_motion = b.x * in_motion[0] + b.y * in_motion[1] + b.z * in_motion[2];
    out_shadow = (draw.light * vec4(world, 1.0)).xyz;
#ifdef TL_WOUNDS
    for (int k = 0; k < 4; k++) {
        out_wound[k] = b.x * in_wound[0][k] + b.y * in_wound[1][k] + b.z * in_wound[2][k];
        out_wound_facing[k] = b.x * in_wound_facing[0][k] + b.y * in_wound_facing[1][k] + b.z * in_wound_facing[2][k];
    }
    out_wound_look = in_wound_look;
    out_wound_tex = in_wound_tex;
    out_wound_run = in_wound_run;
#endif
}
