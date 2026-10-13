#version 450
// A full-screen triangle, for showing a render target in the window.
layout(location = 0) out vec2 out_uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    out_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
