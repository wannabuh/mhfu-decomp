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
    // (the scenery's layers blended over one another - the sand's patches on the sand - lie a hair apart: each cast
    // on the one under it and on itself, dark patches and broad bands of shade on open ground. Such a layer casts
    // from 40 units further from the sun: on nothing that near it, as the leaves of a tree still do on the ground)
    if ((draw.flags & DRAW_BLENDED) != 0u && (draw.flags & DRAW_LIT) == 0u)
        gl_Position.z += 40.0 * length(vec3(draw.light[0][2], draw.light[1][2], draw.light[2][2]));
    out_uv = tl_uv(i);
    out_alpha = tl_alpha(i);
}
