#version 450
#extension GL_ARB_shader_viewport_layer_array : require
// The point lights' shadows: what casts them, drawn into the faces of their cube maps - instance i into face i % 6
// of light i / 6 (layer i). A face is laid out as the cube map is looked up (the major axis, then s and t): the clip
// position is (s, t) over the major axis, so that what is behind the face is clipped; the depth is written by the
// fragment shader (the distance from the light).
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 4) in vec3 in_world;

layout(push_constant) uniform Draw {
    vec2 tex_size;
    uint tex_index, flags, alpha_test, env_color, fog_color, light_set;
    mat4 light;
    uint points;
} draw;

struct LightSet {
    vec4 dir[4];
    vec4 color[4];
    uint count, sun, pad0, pad1;
};
layout(std430, set = 0, binding = 2) readonly buffer Lights { LightSet sets[]; };

layout(location = 0) out vec2 out_uv;
layout(location = 1) out float out_alpha;
layout(location = 2) out vec3 out_rel;
layout(location = 3) flat out float out_reach;

void main() {
    uint light = uint(gl_InstanceIndex) / 6u, face = uint(gl_InstanceIndex) % 6u;
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
    gl_Layer = gl_InstanceIndex;
    out_uv = in_uv;
    out_alpha = in_color.a;
    out_rel = v;
    out_reach = p.w;
}
