#include "shared.h"
// Ambient occlusion (render_ao.c): what its passes share - the world's depth and how to place a pixel of it in the
// camera's space.
layout(set = 0, binding = 0) uniform sampler2D source;  // the pass's input (the depth, or the occlusion)
layout(set = 0, binding = 1) uniform sampler2D depth;   // the render target's depth (full resolution)
layout(set = 0, binding = 2) uniform sampler2D water_mask;  // what shows (full resolution): water 1, foam 0.5, else 0
layout(set = 0, binding = 3) uniform sampler2D under;   // the depth as it was before the water (render_water.c)

layout(push_constant) uniform Ao { AO_PUSH } pc;

vec3 place(ivec2 pix, float d) {
    vec4 v = pc.to_view * vec4((vec2(pix) + 0.5) / pc.params.z, d, 1.0);
    return v.xyz / v.w;
}

// Foam over this much water (farther down its line of sight) is on the water; less, on the shore
const float FOAM_ON_WATER = 30.0;

// The water and the foam on it (light added by flat planes) write depth, in planes stacked at different heights whose
// edges are no surfaces. Water takes no occlusion and gives none. Foam is seen through: what counts is what is under it
// (the shore keeps its occlusion), unless that is the depths: then it is water too (#24).
bool is_water(ivec2 pix) {
    float m = texelFetch(water_mask, pix, 0).r;
    if (m < 0.25)
        return false;
    if (m > 0.75)
        return true;
    return length(place(pix, texelFetch(under, pix, 0).r)) - length(place(pix, texelFetch(depth, pix, 0).r)) > FOAM_ON_WATER;
}

// A pixel's depth, as occlusion takes it
float depth_at(ivec2 pix) {
    float m = texelFetch(water_mask, pix, 0).r;
    return m > 0.25 && m < 0.75 ? texelFetch(under, pix, 0).r : texelFetch(depth, pix, 0).r;
}

// A pixel of the render target (full resolution), in the camera's space
vec3 view_pos(ivec2 pix) {
    return place(pix, depth_at(pix));
}
