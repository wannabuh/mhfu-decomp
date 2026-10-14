#version 450
#extension GL_EXT_nonuniform_qualifier : require
// The GE's pixel stage before blending: the texture function, colour doubling, the alpha test. Blending, depth and
// write masks are the fixed-function state set per draw.
layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 0, binding = 1) uniform sampler2DShadow shadow_map;

layout(push_constant) uniform Draw {
    vec2 tex_size;       // texels, to normalise the coordinates
    uint tex_index;      // into textures[]; ~0u: untextured
    uint flags;          // bits 0-2 texture function, 3 texture alpha, 4 colour doubling, 5 alpha test, 6 clear mode,
                         // 7 fog, 8 in the world (shadows fall on it), 9 lit by the GE (its sun part is known)
    uint alpha_test;     // function (bits 0-2), reference (8-15), mask (16-23)
    uint env_color;      // RGB, for the blend texture function
    uint fog_color;      // RGB
    uint pad;
    mat4 light;
} draw;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) in float in_fog;
layout(location = 3) in vec3 in_shadow;
layout(location = 4) in vec3 in_sun;
layout(location = 0) out vec4 out_color;

// How much the sun is hidden here (0 lit, 1 in shadow): the shadow map's depths compared around the point (3 x 3),
// fading out towards the map's edges
float shadow() {
    vec2 uv = in_shadow.xy * 0.5 + 0.5;
    float edge = max(abs(in_shadow.x), abs(in_shadow.y));
    if (edge >= 1.0 || in_shadow.z <= 0.0 || in_shadow.z >= 1.0)
        return 0.0;
    vec2 texel = 1.0 / vec2(textureSize(shadow_map, 0));
    float ref = in_shadow.z - 0.0004;
    float lit = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            lit += texture(shadow_map, vec3(uv + vec2(x, y) * texel, ref));
    return (1.0 - lit / 9.0) * clamp((1.0 - edge) * 8.0, 0.0, 1.0);
}

void main() {
    vec4 c = in_color;
    float sh = (draw.flags & 256u) != 0u ? shadow() : 0.0;
    if ((draw.flags & 512u) != 0u)          // lit: the sun's part goes in shadow
        c.rgb = max(c.rgb - in_sun * sh, vec3(0.0));
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
    if ((draw.flags & 512u) == 0u)          // lit beforehand (the scenery's colours): darker in shadow
        c.rgb *= 1.0 - 0.45 * sh;
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
