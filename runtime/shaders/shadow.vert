#version 450
#include "shared.h"
// The shadow map: what is drawn in the world, seen from the sun (an orthographic view: draw.light).
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 4) in vec3 in_world;

layout(push_constant) uniform Draw { DRAW_PUSH } draw;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out float out_alpha;

void main() {
    gl_Position = vec4((draw.light * vec4(in_world, 1.0)).xyz, 1.0);
    // (the scenery's layers blended over one another - the sand's patches on the sand - lie a hair apart: each cast
    // on the one under it and on itself, dark patches and broad bands of shade on open ground. Such a layer casts
    // from 40 units further from the sun: on nothing that near it, as the leaves of a tree still do on the ground)
    if ((draw.flags & DRAW_BLENDED) != 0u && (draw.flags & DRAW_LIT) == 0u)
        gl_Position.z += 40.0 * length(vec3(draw.light[0][2], draw.light[1][2], draw.light[2][2]));
    out_uv = in_uv;
    out_alpha = in_color.a;
}
