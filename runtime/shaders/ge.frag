#version 450
#extension GL_EXT_nonuniform_qualifier : require
// The GE's pixel stage before blending: the texture function, colour doubling, the alpha test. Blending, depth and
// write masks are the fixed-function state set per draw.
layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(push_constant) uniform Draw {
    vec2 tex_size;       // texels, to normalise the coordinates
    uint tex_index;      // into textures[]; ~0u: untextured
    uint flags;          // bits 0-2 texture function, 3 texture alpha, 4 colour doubling, 5 alpha test, 6 clear mode,
                         // 7 fog
    uint alpha_test;     // function (bits 0-2), reference (8-15), mask (16-23)
    uint env_color;      // RGB, for the blend texture function
    uint fog_color;      // RGB
} draw;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) in float in_fog;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 c = in_color;
    if ((draw.flags & 64u) == 0u && draw.tex_index != 0xFFFFFFFFu) {
        vec4 t = texture(textures[nonuniformEXT(draw.tex_index)], in_uv / draw.tex_size);
        uint fn = draw.flags & 7u;
        bool use_alpha = (draw.flags & 8u) != 0u;
        vec3 env = vec3(float(draw.env_color & 255u), float((draw.env_color >> 8) & 255u),
                        float((draw.env_color >> 16) & 255u)) / 255.0;
        if (fn == 0u) {                     // modulate
            c = vec4(c.rgb * t.rgb, use_alpha ? c.a * t.a : c.a);
        } else if (fn == 1u) {              // decal
            c = vec4(mix(c.rgb, t.rgb, t.a), c.a);
        } else if (fn == 2u) {              // blend with the environment colour
            c = vec4(mix(c.rgb, env, t.rgb), use_alpha ? c.a * t.a : c.a);
        } else if (fn == 3u) {              // replace
            c = vec4(t.rgb, use_alpha ? t.a : c.a);
        } else {                            // add
            c = vec4(min(c.rgb + t.rgb, vec3(1.0)), use_alpha ? c.a * t.a : c.a);
        }
        if ((draw.flags & 16u) != 0u)
            c.rgb = min(c.rgb * 2.0, vec3(1.0));
    }
    if ((draw.flags & 128u) != 0u) {      // fog: towards the fog colour as the factor falls from 1
        vec3 fog = vec3(float(draw.fog_color & 255u), float((draw.fog_color >> 8) & 255u),
                        float((draw.fog_color >> 16) & 255u)) / 255.0;
        c.rgb = mix(fog, c.rgb, clamp(in_fog, 0.0, 1.0));
    }
    if ((draw.flags & 32u) != 0u) {
        uint fn = draw.alpha_test & 7u, ref = (draw.alpha_test >> 8) & 255u, mask = (draw.alpha_test >> 16) & 255u;
        uint a = uint(c.a * 255.0 + 0.5) & mask;
        ref &= mask;
        bool pass = fn == 1u || (fn == 2u && a == ref) || (fn == 3u && a != ref) || (fn == 4u && a < ref) ||
                    (fn == 5u && a <= ref) || (fn == 6u && a > ref) || (fn == 7u && a >= ref);
        if (!pass)
            discard;
    }
    out_color = c;
}
