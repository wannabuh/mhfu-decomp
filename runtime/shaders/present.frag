#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Present { vec2 scale; } pc;     // the displayed part of the render target
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = vec4(texture(source, in_uv * pc.scale).rgb, 1.0);
}
