#include "shared.h"
// Ambient occlusion (render_ao.c): what its passes share - the world's depth and how to place a pixel of it in the
// camera's space.
layout(set = 0, binding = 0) uniform sampler2D source;  // the pass's input (the depth, or the occlusion)
layout(set = 0, binding = 1) uniform sampler2D depth;   // the render target's depth (full resolution)

layout(push_constant) uniform Ao { AO_PUSH } pc;

// A pixel of the render target (full resolution), in the camera's space
vec3 view_pos(ivec2 pix) {
    float d = texelFetch(depth, pix, 0).r;
    vec4 v = pc.to_view * vec4((vec2(pix) + 0.5) / pc.params.z, d, 1.0);
    return v.xyz / v.w;
}
