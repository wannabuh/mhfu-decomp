#version 450
#extension GL_EXT_nonuniform_qualifier : require
#include "shared.h"
// The shadow map's depth: what the alpha test drops (leaves, fences) casts no shadow.
layout(set = 0, binding = BIND_TEXTURES) uniform sampler2D textures[];

layout(push_constant) uniform Draw { DRAW_PUSH } draw;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in float in_alpha;

void main() {
    bool blended = (draw.flags & DRAW_BLENDED) != 0u;      // blended by its alpha: casts where more than half opaque
    if ((draw.flags & DRAW_ALPHA_TEST) == 0u && !blended)
        return;
    float a = in_alpha;
    if (draw.tex_index != 0xFFFFFFFFu && (draw.flags & DRAW_TEX_ALPHA) != 0u) {
        float t = texture(textures[nonuniformEXT(draw.tex_index)], in_uv / draw.tex_size).a;
        a = (draw.flags & DRAW_TFUNC) == 3u ? t : a * t;
    }
    if (blended && a < 0.5)
        discard;
    if ((draw.flags & DRAW_ALPHA_TEST) == 0u)
        return;
    uint fn = draw.alpha_test & 7u, ref = (draw.alpha_test >> 8) & 255u, mask = (draw.alpha_test >> 16) & 255u;
    uint ai = uint(a * 255.0 + 0.5) & mask;
    ref &= mask;
    bool pass = fn == 1u || (fn == 2u && ai == ref) || (fn == 3u && ai != ref) || (fn == 4u && ai < ref) ||
                (fn == 5u && ai <= ref) || (fn == 6u && ai > ref) || (fn == 7u && ai >= ref);
    if (!pass)
        discard;
}
