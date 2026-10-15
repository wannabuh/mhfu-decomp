#version 450
// The picture onto the window. Larger than the window (a high internal resolution), each pixel the average of all the
// picture's it covers - bilinear taps spread over its footprint - so that the extra resolution smooths edges and
// textures (supersampling) rather than being skipped
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Present { vec2 scale; } pc;     // the displayed part of the render target
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
void main() {
    vec2 uv = in_uv * pc.scale;
    vec2 size = vec2(textureSize(source, 0));
    vec2 foot = vec2(abs(dFdx(uv.x)), abs(dFdy(uv.y))) * size;      // the picture's pixels across this one
    if (foot.x <= 1.0 && foot.y <= 1.0) {
        out_color = vec4(texture(source, uv).rgb, 1.0);
        return;
    }
    // taps two pixels apart (each bilinear: the four about it), enough to cover the footprint
    ivec2 n = clamp(ivec2(ceil(foot * 0.5)), ivec2(1), ivec2(4));
    vec3 sum = vec3(0.0);
    for (int y = 0; y < n.y; y++)
        for (int x = 0; x < n.x; x++) {
            vec2 o = (vec2(x, y) + 0.5) / vec2(n) - 0.5;
            sum += texture(source, uv + o * foot / size).rgb;
        }
    out_color = vec4(sum / float(n.x * n.y), 1.0);
}
