#version 450
#extension GL_EXT_nonuniform_qualifier : require
// The shadow map's depth: what the alpha test drops (leaves, fences) casts no shadow.
layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(push_constant) uniform Draw {
    vec2 tex_size;
    uint tex_index, flags, alpha_test, env_color, fog_color, light_set;
    mat4 light;
} draw;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in float in_alpha;

void main() {
    bool blended = (draw.flags & 2048u) != 0u;      // blended by its alpha: casts where more than half opaque
    if ((draw.flags & 32u) == 0u && !blended)
        return;
    float a = in_alpha;
    if (draw.tex_index != 0xFFFFFFFFu && (draw.flags & 8u) != 0u) {
        float t = texture(textures[nonuniformEXT(draw.tex_index)], in_uv / draw.tex_size).a;
        a = (draw.flags & 7u) == 3u ? t : a * t;
    }
    if (blended && a < 0.5)
        discard;
    if ((draw.flags & 32u) == 0u)
        return;
    uint fn = draw.alpha_test & 7u, ref = (draw.alpha_test >> 8) & 255u, mask = (draw.alpha_test >> 16) & 255u;
    uint ai = uint(a * 255.0 + 0.5) & mask;
    ref &= mask;
    bool pass = fn == 1u || (fn == 2u && ai == ref) || (fn == 3u && ai != ref) || (fn == 4u && ai < ref) ||
                (fn == 5u && ai <= ref) || (fn == 6u && ai > ref) || (fn == 7u && ai >= ref);
    if (!pass)
        discard;
}
