#version 450
#extension GL_EXT_nonuniform_qualifier : require
// The GE's pixel stage before blending: the texture function, colour doubling, the alpha test. Blending, depth and
// write masks are the fixed-function state set per draw.
layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 0, binding = 1) uniform sampler2DShadow shadow_map;

// Lighting by the pixel: a draw's directional lights, in the world (direction to the light, w a power of the diffuse
// term or 0; colour), and which is the sun (the shadows')
struct LightSet {
    vec4 dir[4];
    vec4 color[4];
    uint count, sun, pad0, pad1;
};
layout(std430, set = 0, binding = 2) readonly buffer Lights { LightSet sets[]; };
// The strongest two point lights' shadows: distance from the light over its reach, by direction from it
layout(set = 0, binding = 3) uniform samplerCube point_shadow[2];

// How much of point light i (one of the first two) reaches this point: its cube map's distances compared around the
// direction (4 taps across it)
float point_lit(uint i, vec3 v, float reach) {
    float d = length(v) / reach * 0.96 - 0.01;     // (surfaces at a grazing angle to the light shade themselves)
    vec3 n = normalize(v);
    vec3 a = normalize(cross(n, abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, a);
    float r = 0.006;
    float lit = 0.0;
    lit += d <= texture(point_shadow[i], n + a * r).r ? 1.0 : 0.0;
    lit += d <= texture(point_shadow[i], n - a * r).r ? 1.0 : 0.0;
    lit += d <= texture(point_shadow[i], n + b * r).r ? 1.0 : 0.0;
    lit += d <= texture(point_shadow[i], n - b * r).r ? 1.0 : 0.0;
    return lit * 0.25;
}

layout(push_constant) uniform Draw {
    vec2 tex_size;       // texels, to normalise the coordinates
    uint tex_index;      // into textures[]; ~0u: untextured
    uint flags;          // bits 0-2 texture function, 3 texture alpha, 4 colour doubling, 5 alpha test, 6 clear mode,
                         // 7 fog, 8 in the world (shadows fall on it), 9 lit by the GE (its sun part is known),
                         // 10 lit by the pixel (light_set), 11 (shadow passes) blended, 12 lit beforehand with
                         // normals made from its triangles (shaded by the sun), 13 relief from its texture's
                         // brightness
    uint alpha_test;     // function (bits 0-2), reference (8-15), mask (16-23)
    uint env_color;      // RGB, for the blend texture function
    uint fog_color;      // RGB
    uint light_set;
    mat4 light;
    uint points;         // the point lights' first set of two (~0u: none): dir = place and reach, colour
} draw;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) in float in_fog;
layout(location = 3) in vec3 in_shadow;
layout(location = 4) in vec3 in_sun;
layout(location = 5) in vec3 in_normal;
layout(location = 6) in vec3 in_world;
layout(location = 8) in vec3 in_motion;
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

// The point lights here (the game has none: they are made from its fires): the light falling on the point, by
// distance (smoothly to nothing at the reach), and turned towards it if the normal is known
// How much the point lights' shadows darken here (set by point_light): the game's own lighting near a fire has its
// light in it already, so a shadow takes some of that away too, as much as the light would give
float point_shadow_dark = 0.0;

vec3 point_light(vec3 n, bool has_normal) {
    vec3 sum = vec3(0.0);
    point_shadow_dark = 0.0;
    for (uint s = 0u; s < 2u; s++) {
        uint set = draw.points + s;
        uint count = sets[set].count;
        for (uint i = 0u; i < count; i++) {
            vec4 p = sets[set].dir[i];
            vec3 l = p.xyz - in_world;
            float d = length(l);
            float r = clamp(d / p.w, 0.0, 1.0);    // a window: strong to half way, nothing at the reach
            float k = (1.0 - r * r) * (1.0 - r * r);
            if (has_normal && d > 0.0)
                k *= max(dot(n, l / d), 0.0);
            if (s == 0u && i < sets[set].pad0 && k > 0.0) {
                float lit = point_lit(i, -l, p.w);
                vec3 col = sets[set].color[i].rgb;
                point_shadow_dark += max(col.r, max(col.g, col.b)) * k * (1.0 - lit);
                k *= lit;
            }
            sum += sets[set].color[i].rgb * k;
        }
    }
    return sum;
}

// Relief: the normal tilted by the texture's brightness as a height (its slope on the screen, carried onto the surface:
// Mikkelsen, "Bump Mapping Unparametrized Surfaces on the GPU")
const float RELIEF_HEIGHT = 4.0;       // world units, black to white

vec3 relief_normal(vec3 n, float h) {
    vec3 dpdx = dFdx(in_world), dpdy = dFdy(in_world);
    float dhdx = dFdx(h) * RELIEF_HEIGHT, dhdy = dFdy(h) * RELIEF_HEIGHT;
    vec3 r1 = cross(dpdy, n), r2 = cross(n, dpdx);
    float det = dot(dpdx, r1);
    vec3 grad = sign(det) * (dhdx * r1 + dhdy * r2);
    vec3 b = abs(det) * n - grad;
    return dot(b, b) > 0.0 ? normalize(b) : n;
}

void main() {
    vec4 c = in_color;
    // the texture (sampled here: its slopes on the screen need every pixel's, before any is discarded)
    bool textured = (draw.flags & 64u) == 0u && draw.tex_index != 0xFFFFFFFFu;
    vec4 t = textured ? texture(textures[nonuniformEXT(draw.tex_index)], in_uv / draw.tex_size) : vec4(1.0);
    vec3 bumped = (draw.flags & 8192u) != 0u ? relief_normal(normalize(in_normal + vec3(0.0, 1e-6, 0.0)),
                                                             dot(t.rgb, vec3(0.299, 0.587, 0.114))) : vec3(0.0);
    bool points = draw.points != 0xFFFFFFFFu && (draw.flags & 256u) != 0u;
    float sh = (draw.flags & 256u) != 0u ? shadow() : 0.0;
    if ((draw.flags & 512u) != 0u)          // lit: the sun's part goes in shadow
        c.rgb = max(c.rgb - in_sun * sh, vec3(0.0));
    if ((draw.flags & 1024u) != 0u) {       // lit by the pixel: the lights' diffuse terms on the material's colour
        vec3 n = in_normal;
        float nl = length(n);
        n = nl > 0.0 ? n / nl : vec3(0.0, 1.0, 0.0);
        vec3 sum = vec3(0.0);
        uint count = sets[draw.light_set].count, sun = sets[draw.light_set].sun;
        for (uint i = 0u; i < count; i++) {
            vec4 d = sets[draw.light_set].dir[i];
            float k = max(dot(n, d.xyz), 0.0);
            if (d.w > 0.0)
                k = pow(k, d.w);
            vec3 term = sets[draw.light_set].color[i].rgb * in_sun * k;
            sum += i == sun ? term * (1.0 - sh) : term;
        }
        if (points)
            sum += in_sun * point_light(n, true);
        c.rgb = min(c.rgb + sum, vec3(1.0));
        if (points)
            c.rgb *= 1.0 - clamp(point_shadow_dark * 0.6, 0.0, 0.6);
    }
    if (textured) {
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
    if ((draw.flags & 1536u) == 0u) {       // lit beforehand (the scenery's colours): darker in shadow - and turned
        float dark = sh;                    // away from the sun, as dark as in shadow
        float nl = length(in_normal);
        float relief = 1.0;
        if ((draw.flags & 4096u) != 0u && nl > 0.0) {
            vec3 sun = -normalize(vec3(draw.light[0][2], draw.light[1][2], draw.light[2][2]));
            float lambert = dot(in_normal / nl, sun);
            float facing = smoothstep(-0.1, 0.3, lambert);
            dark = 1.0 - (1.0 - sh) * facing;
            // the relief, in the sun: lighter where it turns towards it, darker where away
            if ((draw.flags & 8192u) != 0u)
                relief = mix(1.0, clamp(dot(bumped, sun) / max(lambert, 0.25), 0.65, 1.35), 1.0 - dark);
        }
        c.rgb *= (1.0 - 0.45 * dark) * relief;
    }
    if (points && (draw.flags & 1024u) == 0u)   // not lit by the pixel: the point lights brighten its colour
    {
        c.rgb = min(c.rgb * (vec3(1.0) + point_light(vec3(0.0), false)), vec3(1.0));
        c.rgb *= 1.0 - clamp(point_shadow_dark * 0.6, 0.0, 0.6);
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
    if ((draw.flags & 16384u) != 0u) {  // drawn again into the motion blur's (post.c): its motion in the world, covered
        out_color = vec4(in_motion, 1.0);
        return;
    }
    out_color = c;
}
