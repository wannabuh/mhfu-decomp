#version 450
#extension GL_EXT_nonuniform_qualifier : require
#include "shared.h"
// The point lights' shadow maps: the distance from the light over its reach; what the alpha test drops casts nothing.
layout(set = 0, binding = BIND_TEXTURES) uniform sampler2D textures[];

layout(push_constant) uniform Draw { DRAW_PUSH } draw;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in float in_alpha;
layout(location = 2) in vec3 in_rel;
layout(location = 3) flat in float in_reach;

void main() {
    // the fire's own logs and stones (the light is among them) cast nothing - the scenery only: who comes close
    // (models the game lights) still casts
    if (length(in_rel) < 160.0 && (draw.flags & DRAW_LIT) == 0u)
        discard;
    bool blended = (draw.flags & DRAW_BLENDED) != 0u;
    if ((draw.flags & DRAW_ALPHA_TEST) != 0u || blended) {
        float a = in_alpha;
        if (draw.tex_index != 0xFFFFFFFFu && (draw.flags & DRAW_TEX_ALPHA) != 0u) {
            float t = texture(textures[nonuniformEXT(draw.tex_index)], in_uv / draw.tex_size).a;
            a = (draw.flags & DRAW_TFUNC) == 3u ? t : a * t;
        }
        if (blended && a < 0.5)
            discard;
        if ((draw.flags & DRAW_ALPHA_TEST) != 0u) {
            uint fn = draw.alpha_test & 7u, ref = (draw.alpha_test >> 8) & 255u, mask = (draw.alpha_test >> 16) & 255u;
            uint ai = uint(a * 255.0 + 0.5) & mask;
            ref &= mask;
            bool pass = fn == 1u || (fn == 2u && ai == ref) || (fn == 3u && ai != ref) || (fn == 4u && ai < ref) ||
                        (fn == 5u && ai <= ref) || (fn == 6u && ai > ref) || (fn == 7u && ai >= ref);
            if (!pass)
                discard;
        }
    }
    gl_FragDepth = clamp(length(in_rel) / in_reach, 0.0, 1.0);
}
