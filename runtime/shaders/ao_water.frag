#version 450
// Ambient occlusion's mask of the water and foam (render_ao.c): their draws again, against the world's finished depth, so
// that what is marked is what nothing was drawn in front of. Water 1, foam (seen through) 0.5.
#include "shared.h"
layout(push_constant) uniform Draw { DRAW_PUSH } draw;
layout(location = 0) out vec4 out_mask;

void main() {
    out_mask = vec4((draw.flags & DRAW_WATER) != 0u ? 1.0 : 0.5);
}
