// The GE's vertex stage on the GPU (MHFU_GPU_TL): what its shaders share (ge_tl.vert, shadow_tl.vert,
// pshadow_tl.vert) - the vertices as the game gave them, decoded; the draws (TlDraw, the instance); the vertex placed.
#include "shared.h"
#define RT_HALF 256.0           // (render_internal.h RT_SIZE / 2)
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_w0;
layout(location = 5) in vec4 in_w1;
layout(location = 6) in vec3 in_shape;
layout(location = 7) in uint in_edges;

layout(std430, set = 0, binding = BIND_TL) readonly buffer Draws { TlDraw draws[]; };
layout(std430, set = 0, binding = BIND_TL_EXTRA) readonly buffer Extra { vec4 extra[]; };
layout(push_constant) uniform Draw { DRAW_PUSH } draw;

// (3 x 4 by rows) a point, a direction
vec3 xf(vec4 a, vec4 b, vec4 c, vec3 p) {
    vec4 q = vec4(p, 1.0);
    return vec3(dot(a, q), dot(b, q), dot(c, q));
}
vec3 xf3(vec4 a, vec4 b, vec4 c, vec3 n) {
    return vec3(dot(a.xyz, n), dot(b.xyz, n), dot(c.xyz, n));
}

// C's powf where pow is undefined (nothing to a power: 0, or 1 to the 0th)
float powc(float x, float y) {
    return x > 0.0 ? pow(x, y) : (y == 0.0 ? 1.0 : 0.0);
}

float weight(int b) {
    return b < 4 ? in_w0[b] : in_w1[b - 4];
}

// The vertex skinned (and its normal, turned round if the draw says), as bones b give it
void tl_skin(uint i, bool earlier, inout vec3 p, inout vec3 nr) {
    int nb = int(draws[i].bones);
    if (nb == 0)
        return;
    vec3 acc = vec3(0.0), nacc = vec3(0.0);
    for (int b = 0; b < nb; b++) {
        vec4 r0 = earlier ? draws[i].earlier_bone[3 * b] : draws[i].bone[3 * b];
        vec4 r1 = earlier ? draws[i].earlier_bone[3 * b + 1] : draws[i].bone[3 * b + 1];
        vec4 r2 = earlier ? draws[i].earlier_bone[3 * b + 2] : draws[i].bone[3 * b + 2];
        acc += xf(r0, r1, r2, p) * weight(b);
        nacc += xf3(r0, r1, r2, nr) * weight(b);
    }
    p = acc;
    nr = nacc;
}

vec3 tl_world(uint i, vec3 m) {
    return xf(draws[i].to_world[0], draws[i].to_world[1], draws[i].to_world[2], m);
}

// The wind (render_shading.c sway_batch): the vertex at m (the GE's world space) moved, if the draw sways - by its
// height above its feet (BIND_TL_EXTRA), and bent away from the feet among the plants
vec3 tl_sway(uint i, vec3 m) {
    if ((draw.flags & DRAW_SWAY) == 0u)
        return m;
    float h = extra[2u * TL_WIND + uint(gl_VertexIndex)].x;
    if (h <= 0.5)
        return m;
    vec3 w = tl_world(i, m);
    vec4 wind = extra[draw.camera];     // (time, how far, how many feet)
    float t = wind.x, foot = w.y - h;
    float wgt = min(max(h - 10.0, 0.0) / 80.0, 1.0);     // (small cards hardly)
    wgt *= sqrt(wgt);
    float ph = w.x * 0.0041 + w.z * 0.0033;
    float gust = 0.55 + 0.45 * sin(t * 0.37 + w.x * 0.0006 - w.z * 0.0004);
    float sw = 0.7 * sin(t * 1.9 + ph * 6.2832) + 0.3 * sin(t * 3.7 + ph * 11.0);
    float d = (sw + 0.4) * gust * wind.y * wgt;
    vec3 dw = vec3(0.8 * d, -0.1 * abs(d), 0.6 * d);
    for (uint f = 0u; f < uint(wind.z); f++) {  // bent away from feet among them: out and down, the more the nearer
        vec4 ft = extra[draw.camera + 1u + f];
        float dx = w.x - ft.x, dz = w.z - ft.z, reach = ft.w + 45.0;
        if (abs(dx) > reach || abs(dz) > reach || abs(foot - ft.y) > 60.0)
            continue;
        float dist = sqrt(dx * dx + dz * dz);
        if (dist >= reach || dist < 1e-3)
            continue;
        float push = (1.0 - dist / reach) * min(h, 60.0) * 0.6;
        dw += vec3(dx / dist * push, -push * 0.5, dz / dist * push);
    }
    // (the world's move, in the GE's world space: to_world's turn undone)
    mat3 turn = transpose(mat3(draws[i].to_world[0].xyz, draws[i].to_world[1].xyz, draws[i].to_world[2].xyz));
    return m + inverse(turn) * dw;
}

// Where the vertex is in the GE's world space (the world matrix's: the game's camera in it; moved by the wind)
vec3 tl_place(uint i) {
    vec3 p = in_pos, nr = vec3(0.0);
    tl_skin(i, false, p, nr);
    return tl_sway(i, xf(draws[i].world[0], draws[i].world[1], draws[i].world[2], p));
}

// Its texture coordinates (texels) and alpha (as light() and put() make it, lit)
vec2 tl_uv(uint i) {
    return (draws[i].flags & TL_TEX) != 0u ? in_uv * draws[i].tex.xy + draws[i].tex.zw : vec2(0.0);
}
float tl_alpha(uint i) {
    if ((draws[i].flags & TL_LIT) == 0u)
        return in_color.a;
    float a = (draws[i].mode & 1u) != 0u ? in_color.a : draws[i].ambient.w;
    return floor(clamp(a * draws[i].global.w * 255.0, 0.0, 255.0)) / 255.0;
}
