#version 450
// GE vertices, transformed (and lit) on the CPU: clip-space position, texture coordinates in texels, colour; for
// shadows, the position in the world and the part of the colour the sun gave (lit by the pixel: the material's
// diffuse colour, and the normal in the world).
layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in float in_fog;
layout(location = 4) in vec3 in_world;
layout(location = 5) in vec4 in_sun;
layout(location = 6) in vec4 in_normal;

layout(push_constant) uniform Draw {
    vec2 tex_size;
    uint tex_index, flags, alpha_test, env_color, fog_color, light_set;
    mat4 light;          // the world to the shadow map: x, y in -1..1, depth 0..1 away from the sun
    uint points;
} draw;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) out float out_fog;
layout(location = 3) out vec3 out_shadow;
layout(location = 4) out vec3 out_sun;
layout(location = 5) out vec3 out_normal;
layout(location = 6) out vec3 out_world;
layout(location = 7) out vec4 out_shape;    // (tessellated: the normal that curves it, and which of its triangle's
                                            // edges stay straight - render.c shape_normals)

void main() {
    gl_Position = in_pos;
    out_uv = in_uv;
    out_color = in_color;
    out_fog = in_fog;
    out_shadow = (draw.light * vec4(in_world, 1.0)).xyz;
    out_sun = in_sun.rgb;
    out_normal = in_normal.xyz;
    out_world = in_world;
    int raw = int(round(in_normal.w * 32767.0));
    out_shape.w = round(in_sun.a * 255.0);
    if (raw == 0) {
        out_shape.xyz = in_normal.xyz;
    } else {                                // octahedral, 8 + 8 bits
        int ex = raw >= 0 ? raw / 256 : -((-raw + 255) / 256);
        vec2 o = vec2(float(ex), float(raw - ex * 256 - 127)) / 127.0;
        vec3 n = vec3(o, 1.0 - abs(o.x) - abs(o.y));
        if (n.z < 0.0)
            n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        out_shape.xyz = normalize(n);
    }
}
