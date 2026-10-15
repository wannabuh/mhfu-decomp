#version 450
// Rounder characters (render.c, MHFU_TESSELLATION): what is lit by the pixel (the hunter, monsters, people - they
// have normals) cut into smaller triangles, as many as their edges are long on the screen, for ge.tese to curve.
layout(vertices = 3) out;

layout(location = 0) in vec2 in_uv[];
layout(location = 1) in vec4 in_color[];
layout(location = 2) in float in_fog[];
layout(location = 3) in vec3 in_shadow[];
layout(location = 4) in vec3 in_sun[];
layout(location = 5) in vec3 in_normal[];
layout(location = 6) in vec3 in_world[];
layout(location = 7) in vec4 in_shape[];
layout(location = 0) out vec2 out_uv[];
layout(location = 1) out vec4 out_color[];
layout(location = 2) out float out_fog[];
layout(location = 3) out vec3 out_shadow[];
layout(location = 4) out vec3 out_sun[];
layout(location = 5) out vec3 out_normal[];
layout(location = 6) out vec3 out_world[];
layout(location = 7) out vec4 out_shape[];

const float SEGMENT = 2.5;      // PSP pixels a piece of an edge, at most (the screen is 512 across its -1..1)

// an edge's pieces: by its length on the screen
float pieces(vec4 a, vec4 b) {
    if (a.w <= 0.0 || b.w <= 0.0)
        return 1.0;
    float len = length(a.xy / a.w - b.xy / b.w) * 256.0;
    return clamp(len / SEGMENT, 1.0, 8.0);
}

void main() {
    int i = gl_InvocationID;
    gl_out[gl_InvocationID].gl_Position = gl_in[i].gl_Position;
    out_uv[gl_InvocationID] = in_uv[i];
    out_color[gl_InvocationID] = in_color[i];
    out_fog[gl_InvocationID] = in_fog[i];
    out_shadow[gl_InvocationID] = in_shadow[i];
    out_sun[gl_InvocationID] = in_sun[i];
    out_normal[gl_InvocationID] = in_normal[i];
    out_world[gl_InvocationID] = in_world[i];
    out_shape[gl_InvocationID] = in_shape[i];
    if (i == 0) {
        vec4 p0 = gl_in[0].gl_Position, p1 = gl_in[1].gl_Position, p2 = gl_in[2].gl_Position;
        bool normals = dot(in_shape[0].xyz, in_shape[0].xyz) > 0.0 && dot(in_shape[1].xyz, in_shape[1].xyz) > 0.0 &&
                       dot(in_shape[2].xyz, in_shape[2].xyz) > 0.0;
        // (the outer levels: the edges facing corners 0, 1, 2)
        float e0 = normals ? pieces(p1, p2) : 1.0, e1 = normals ? pieces(p2, p0) : 1.0;
        float e2 = normals ? pieces(p0, p1) : 1.0;
        // (an edge another draw goes on from stays whole: cut, it would leave pinholes along that draw's)
        uint open = uint(in_shape[0].w);
        gl_TessLevelOuter[0] = (open & 1u) != 0u ? 1.0 : e0;
        gl_TessLevelOuter[1] = (open & 2u) != 0u ? 1.0 : e1;
        gl_TessLevelOuter[2] = (open & 4u) != 0u ? 1.0 : e2;
        gl_TessLevelInner[0] = max(e0, max(e1, e2));
    }
}
