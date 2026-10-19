#version 450
#extension GL_ARB_shader_viewport_layer_array : require
// The point lights' shadows (pshadow.vert) for draws whose vertex stage is the GPU's (MHFU_GPU_TL): each vertex placed
// in the world as ge_tl.vert places it, drawn into the cube maps' faces as pshadow.vert draws it - the instance its
// record times 12, plus the layer.
#include "tl_common.glsl"

layout(std430, set = 0, binding = BIND_LIGHTS) readonly buffer Lights { LightSet sets[]; };

layout(location = 0) out vec2 out_uv;
layout(location = 1) out float out_alpha;
layout(location = 2) out vec3 out_rel;
layout(location = 3) flat out float out_reach;

void main() {
    uint i = uint(gl_InstanceIndex) / 12u, layer = uint(gl_InstanceIndex) % 12u;
    uint light = layer / 6u, face = layer % 6u;
    vec3 in_world = tl_world(i, tl_place(i));
    vec4 p = sets[draw.points].dir[light];
    vec3 v = in_world - p.xyz;
    float sc, tc, ma;
    if (face == 0u)      { ma = v.x;  sc = -v.z; tc = -v.y; }
    else if (face == 1u) { ma = -v.x; sc = v.z;  tc = -v.y; }
    else if (face == 2u) { ma = v.y;  sc = v.x;  tc = v.z; }
    else if (face == 3u) { ma = -v.y; sc = v.x;  tc = -v.z; }
    else if (face == 4u) { ma = v.z;  sc = v.x;  tc = -v.y; }
    else                 { ma = -v.z; sc = -v.x; tc = -v.y; }
    gl_Position = vec4(sc, tc, 0.5 * ma, ma);
    gl_Layer = int(layer);
    out_uv = tl_uv(i);
    out_alpha = tl_alpha(i);
    out_rel = v;
    out_reach = p.w;
}
