#version 450
// The shadow map: what is drawn in the world, seen from the sun (an orthographic view: draw.light).
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 4) in vec3 in_world;

layout(push_constant) uniform Draw {
    vec2 tex_size;
    uint tex_index, flags, alpha_test, env_color, fog_color, light_set;
    mat4 light;
} draw;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out float out_alpha;

void main() {
    gl_Position = vec4((draw.light * vec4(in_world, 1.0)).xyz, 1.0);
    out_uv = in_uv;
    out_alpha = in_color.a;
}
