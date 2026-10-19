#version 450
// The shadow map (shadow.vert) for draws whose vertex stage is the GPU's (MHFU_GPU_TL): each vertex placed in the world
// as ge_tl.vert places it - whole triangles, whatever the camera's near plane cuts off - seen from the sun.
#include "tl_common.glsl"

layout(location = 0) out vec2 out_uv;
layout(location = 1) out float out_alpha;

void main() {
    uint i = uint(gl_InstanceIndex);
    vec3 world = tl_world(i, tl_place(i));
    gl_Position = vec4((draw.light * vec4(world, 1.0)).xyz, 1.0);
    out_uv = tl_uv(i);
    out_alpha = tl_alpha(i);
}
