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
    out_uv = in_uv;
    out_alpha = in_color.a;
}
