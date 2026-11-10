// The GE's pixel stage before blending (ge.frag; ge_tl.frag, the GPU's vertex stage's, with TL_CORNERS): the texture
// function, colour doubling, the alpha test. Blending, depth and write masks are the fixed-function state set per draw.
#extension GL_EXT_nonuniform_qualifier : require
#include "shared.h"
layout(set = 0, binding = BIND_TEXTURES) uniform sampler2D textures[];
layout(set = 0, binding = BIND_SHADOW_MAP) uniform sampler2DShadow shadow_map;

// Lighting by the pixel: a draw's directional lights, in the world (direction to the light, w a power of the diffuse
// term or 0; colour), and which is the sun (the shadows')
layout(std430, set = 0, binding = BIND_LIGHTS) readonly buffer Lights { LightSet sets[]; };
// The strongest two point lights' shadows: distance from the light over its reach, by direction from it
layout(set = 0, binding = BIND_POINT_SHADOWS) uniform samplerCube point_shadow[2];

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

layout(push_constant) uniform Draw { DRAW_PUSH } draw;

// Water (render_water.c): the picture under it as it was before the water, colour and depth
layout(set = 0, binding = BIND_SCENE_COLOR) uniform sampler2D scene_color;
layout(set = 0, binding = BIND_SCENE_DEPTH) uniform sampler2D scene_depth;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) in float in_fog;
layout(location = 3) in vec3 in_shadow;
layout(location = 4) in vec3 in_sun;
layout(location = 5) in vec3 in_normal;
#ifdef TL_CORNERS
layout(location = 9) pervertexEXT in vec3 in_corner[];     // (the scenery's: each corner's normal summed at its place)
layout(location = 10) in vec3 in_wound[4];                 // (ge_tl.vert: each wound's, 1 at its edge)
#endif
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
    // The surface's depth as a plane across the map (from its slopes on the screen), each tap compared with the surface
    // where that tap is: then a surface needs a bias against itself of only a texel's worth of its slope, in world units
    // (a fixed one, with the map's depth range, let a wide line of light in where the ground meets a wall: #25)
    vec3 sx = dFdx(in_shadow), sy = dFdy(in_shadow);
    float unit = length(vec3(draw.light[0][2], draw.light[1][2], draw.light[2][2]));    // depth per world unit
    float det = sx.x * sy.y - sx.y * sy.x;
    vec2 slope = abs(det) > 1e-14 ? vec2(sy.y * sx.z - sx.y * sy.z, sx.x * sy.z - sy.x * sx.z) / det * 2.0 * texel
                                  : vec2(16.0 * unit);     // depth per texel (edge on to the sun: the most allowed)
    slope = clamp(slope, vec2(-16.0 * unit), vec2(16.0 * unit));
    float ref = in_shadow.z - 0.5 * unit - abs(slope.x) - abs(slope.y);
    float lit = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            lit += texture(shadow_map, vec3(uv + vec2(x, y) * texel, ref + dot(vec2(x, y), slope)));
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

// Faded out as the texture is shrunk on the screen (more than a texel a pixel, along either way: far, or seen at a low
// angle) - there its slopes between neighbouring pixels are noise, which shimmered as the camera moved
vec3 relief_normal(vec3 n, float h) {
    vec3 dpdx = dFdxFine(in_world), dpdy = dFdyFine(in_world);
    vec2 duvdx = dFdxFine(in_uv), duvdy = dFdyFine(in_uv);
    float texels = sqrt(max(dot(duvdx, duvdx), dot(duvdy, duvdy)));
    float k = RELIEF_HEIGHT * (1.0 - smoothstep(0.5, 1.5, texels));
    float dhdx = dFdxFine(h) * k, dhdy = dFdyFine(h) * k;
    vec3 r1 = cross(dpdy, n), r2 = cross(n, dpdx);
    float det = dot(dpdx, r1);
    vec3 grad = sign(det) * (dhdx * r1 + dhdy * r2);
    vec3 b = abs(det) * n - grad;
    return dot(b, b) > 0.0 ? normalize(b) : n;
}

// -- water ---------------------------------------------------------------------------------------------------------
// Its light sets (draw.camera): the world to the clip space and back; the eye (w: nearer is a greater depth), the
// sun's colour, the time and the picture's visible part

mat4 water_to_clip() { uint s = draw.camera; return mat4(sets[s].dir[0], sets[s].dir[1], sets[s].dir[2], sets[s].dir[3]); }
mat4 water_to_world() { uint s = draw.camera; return mat4(sets[s].color[0], sets[s].color[1], sets[s].color[2], sets[s].color[3]); }

// A point of the picture (0..1 across the render target, its depth) in the world
vec3 picture_world(mat4 to_world, vec2 uv, float depth) {
    vec4 p = to_world * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return p.xyz / p.w;
}

// Gradient noise and its slope (Quilez's, with a hash for the gradients)
vec2 ripple_hash(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453);
}

vec3 ripple_noise(vec2 p) {         // (value, d/dx, d/dy)
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0), du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    vec2 ga = ripple_hash(i), gb = ripple_hash(i + vec2(1.0, 0.0));
    vec2 gc = ripple_hash(i + vec2(0.0, 1.0)), gd = ripple_hash(i + vec2(1.0, 1.0));
    float va = dot(ga, f), vb = dot(gb, f - vec2(1.0, 0.0));
    float vc = dot(gc, f - vec2(0.0, 1.0)), vd = dot(gd, f - vec2(1.0, 1.0));
    return vec3(va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd),
                ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
                    du * (u.yx * (va - vb - vc + vd) + vec2(vb, vc) - va));
}

// The ripples: two long swells, and noise over them in layers drifting different ways - the surface's normal. What is
// shorter than a few pixels (far) fades out, so it does not shimmer
vec3 water_normal(vec2 p, float t, float far) {
    vec2 g = vec2(0.0);
    const vec4 swells[2] = vec4[2](vec4(0.80, 0.60, 340.0, 40.0), vec4(-0.50, 0.87, 210.0, 31.0));
    for (int i = 0; i < 2; i++) {
        float k = 6.2831853 / swells[i].z;
        g += swells[i].xy * 0.05 * cos(dot(swells[i].xy, p) * k - t * swells[i].w * k) *
             (1.0 - smoothstep(0.12, 0.3, far / swells[i].z));
    }
    const vec4 layers[4] = vec4[4](     // size (world units), drift (units a second, x and z), strength
        vec4(160.0, 14.0, 9.0, 0.10), vec4(90.0, -11.0, 13.0, 0.08), vec4(48.0, 8.0, -12.0, 0.06),
        vec4(25.0, -9.0, -7.0, 0.045));
    for (int i = 0; i < 4; i++) {
        float size = layers[i].x;
        vec3 nz = ripple_noise((p + t * layers[i].yz) / size + float(i) * 17.0);
        g += nz.yz * layers[i].w * (1.0 - smoothstep(0.12, 0.3, far / size));
    }
    return normalize(vec3(-g.x, 1.0, -g.y));
}

// The water's colour here: what is under it (bent by the ripples, tinted the deeper it is), what is above reflected,
// the sun's glint - given the game's colour for it (its tint) and whether the sun is hidden
vec3 water(vec3 tint, float hidden, float far) {
    uint s = draw.camera;
    vec3 eye = sets[s + 1u].dir[0].xyz;
    bool greater = sets[s + 1u].dir[0].w > 0.5;
    vec3 sun_rgb = sets[s + 1u].dir[1].rgb;
    float t = sets[s + 1u].dir[2].x;
    vec2 visible = sets[s + 1u].dir[2].yz;
    mat4 to_clip = water_to_clip(), to_world = water_to_world();
    vec3 p = in_world;
    vec3 v = eye - p;
    float dist = length(v);
    v /= dist;
    vec3 n = water_normal(p.xz, t, far);
    vec2 size = vec2(textureSize(scene_color, 0));
    vec2 uv = gl_FragCoord.xy / size;

    // under it: the picture bent by the ripples (not where that would take something in front of the water)
    vec2 bent = uv + n.xz * 0.035 * clamp(300.0 / dist, 0.15, 1.0);
    bent = clamp(bent, vec2(0.0), visible);
    float bd = texture(scene_depth, bent).r;
    if (greater ? bd > gl_FragCoord.z : bd < gl_FragCoord.z) {
        bent = uv;
        bd = texture(scene_depth, uv).r;
    }
    vec3 under = texture(scene_color, bent).rgb;
    vec3 bottom = picture_world(to_world, bent, bd);
    float depth = (greater ? bd <= 0.0 : bd >= 1.0) ? 1e5 : length(bottom - p);
    vec3 body = tint * 0.75;
    float murk = 1.0 - exp(-depth / 220.0);
    under = mix(under * mix(vec3(1.0), tint * 1.4, 0.35), body, murk * 0.85);

    // above it: the reflection, followed through the picture (marching along it, a hit refined by halving)
    vec3 r = reflect(-v, n);
    if (r.y < 0.02)
        r = normalize(vec3(r.x, 0.02, r.z));
    vec3 fog = vec3(float(draw.fog_color & 255u), float((draw.fog_color >> 8) & 255u),
                    float((draw.fog_color >> 16) & 255u)) / 255.0;
    vec3 sky = mix(fog, vec3(0.62, 0.72, 0.85), 0.35);
    vec3 above = sky;
    float step = max(dist * 0.035, 6.0);
    vec3 q = p + n * 2.0;
    float found = 0.0;
    for (int i = 0; i < 40; i++) {
        vec3 next = q + r * step;
        vec4 c = to_clip * vec4(next, 1.0);
        if (c.w <= 0.0)
            break;
        vec3 ndc = c.xyz / c.w;
        vec2 at = ndc.xy * 0.5 + 0.5;
        if (any(lessThan(at, vec2(0.0))) || any(greaterThan(at, visible)))
            break;
        float sd = texture(scene_depth, at).r;
        bool behind = greater ? ndc.z < sd : ndc.z > sd;
        if (behind) {
            float sdist = length(picture_world(to_world, at, sd) - eye);
            if (length(next - eye) - sdist < step * 2.5) {      // behind it, not far behind (not passing under it)
                vec3 a = q, b = next;
                for (int k = 0; k < 5; k++) {
                    vec3 m = 0.5 * (a + b);
                    vec4 mc = to_clip * vec4(m, 1.0);
                    vec3 mn = mc.xyz / mc.w;
                    float md = texture(scene_depth, mn.xy * 0.5 + 0.5).r;
                    if (greater ? mn.z < md : mn.z > md) b = m; else a = m;
                }
                vec4 bc = to_clip * vec4(b, 1.0);
                vec2 hit = bc.xy / bc.w * 0.5 + 0.5;
                vec2 edge = min(hit, visible - hit) / visible;
                found = clamp(min(edge.x, edge.y) * 12.0, 0.0, 1.0) * (1.0 - float(i) / 40.0);
                above = mix(sky, texture(scene_color, hit).rgb, found);
                break;
            }
        }
        q = next;
        step *= 1.07;
    }

    float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    vec3 col = mix(under, above, clamp(fresnel * 1.15, 0.0, 1.0));
    // the sun's glint
    vec3 sun = -normalize(vec3(draw.light[0][2], draw.light[1][2], draw.light[2][2]));
    float glint = pow(max(dot(r, sun), 0.0), 380.0) * 4.0 + pow(max(dot(r, sun), 0.0), 40.0) * 0.12;
    col += sun_rgb * glint * (1.0 - hidden);
    return col;
}

void main() {
    vec4 c = in_color;
    // the texture (sampled here: its slopes on the screen need every pixel's, before any is discarded)
    bool textured = (draw.flags & DRAW_CLEAR) == 0u && draw.tex_index != 0xFFFFFFFFu;
    vec4 t = textured ? texture(textures[nonuniformEXT(draw.tex_index)], in_uv / draw.tex_size) : vec4(1.0);
    // the scenery's normal (DRAW_FACES) as render_shading.c make_normals has each corner's: the normals summed at its
    // place, or its triangle's own where that bends more than 60 degrees from it - the triangle's turned towards the
    // camera (its slopes on the screen face it). With TL_CORNERS each corner's chosen, then interpolated as the CPU's;
    // without, chosen here for the pixel
    vec3 normal = in_normal;
    if ((draw.flags & DRAW_FACES) != 0u) {
        vec3 f = -cross(dFdx(in_world), dFdy(in_world));
        float fl = length(f);
        if (fl > 0.0) {
            f /= fl;
#ifdef TL_CORNERS
            normal = vec3(0.0);
            for (int k = 0; k < 3; k++) {
                vec3 s = in_corner[k];
                float sl = length(s);
                normal += gl_BaryCoordEXT[k] * (sl > 0.0 && dot(s, f) > 0.5 * sl ? s / sl : f);
            }
#else
            float sl = length(in_normal);
            if (!(sl > 0.0 && dot(in_normal, f) > 0.5 * sl))
                normal = f;
#endif
        }
    }
    vec3 bumped = (draw.flags & DRAW_RELIEF) != 0u ? relief_normal(normalize(normal + vec3(0.0, 1e-6, 0.0)),
                                                             dot(t.rgb, vec3(0.299, 0.587, 0.114))) : vec3(0.0);
    bool points = draw.points != 0xFFFFFFFFu && (draw.flags & DRAW_WORLD) != 0u;
    float sh = (draw.flags & DRAW_WORLD) != 0u ? shadow() : 0.0;
    if ((draw.flags & DRAW_LIT_GE) != 0u)          // lit: the sun's part goes in shadow
        c.rgb = max(c.rgb - in_sun * sh, vec3(0.0));
    if ((draw.flags & DRAW_LIT_PIXEL) != 0u) {       // lit by the pixel: the lights' diffuse terms on the material's colour
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
        uint fn = draw.flags & DRAW_TFUNC;
        bool use_alpha = (draw.flags & DRAW_TEX_ALPHA) != 0u;
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
        if ((draw.flags & DRAW_DOUBLE) != 0u)
            c.rgb = min(c.rgb * 2.0, vec3(1.0));
    }
    if ((draw.flags & DRAW_LIT) == 0u) {       // lit beforehand (the scenery's colours): darker in shadow - and turned
        float dark = sh;                    // away from the sun, as dark as in shadow
        float nl = length(normal);
        float relief = 1.0;
        if ((draw.flags & DRAW_SHADED) != 0u && nl > 0.0) {
            vec3 sun = -normalize(vec3(draw.light[0][2], draw.light[1][2], draw.light[2][2]));
            float lambert = dot(normal / nl, sun);
            float facing = smoothstep(-0.1, 0.3, lambert);
            dark = 1.0 - (1.0 - sh) * facing;
            // the relief, in the sun: lighter where it turns towards it, darker where away
            if ((draw.flags & DRAW_RELIEF) != 0u)
                relief = mix(1.0, clamp(dot(bumped, sun) / max(lambert, 0.25), 0.65, 1.35), 1.0 - dark);
        }
        c.rgb *= (1.0 - 0.45 * dark) * relief;
    }
    if (points && (draw.flags & DRAW_LIT_PIXEL) == 0u)   // not lit by the pixel: the point lights brighten its colour
    {
        c.rgb = min(c.rgb * (vec3(1.0) + point_light(vec3(0.0), false)), vec3(1.0));
        c.rgb *= 1.0 - clamp(point_shadow_dark * 0.6, 0.0, 0.6);
    }
    if ((draw.flags & DRAW_WATER) != 0u && draw.camera != 0xFFFFFFFFu) {
        // water where the surface faces up, below the eye; its shore (the game fading it out) kept
        vec3 ng = cross(dFdx(in_world), dFdy(in_world));
        float far = length(fwidth(in_world.xz));    // world units a pixel spans here (the short waves fade out)
        vec3 eye = sets[draw.camera + 1u].dir[0].xyz;
        if (dot(ng, ng) > 0.0 && abs(normalize(ng).y) > 0.8 && in_world.y < eye.y)
            c = vec4(water(c.rgb, sh, far), clamp(c.a * 3.0, 0.0, 1.0));
    }
#ifdef TL_CORNERS
    // wounds (gpu.c): a slash, dark red, its edges soft; lit as the skin under it is
    for (int k = 0; k < 4; k++) {
        float s = length(in_wound[k]);
        if (s < 1.0) {
            float m = smoothstep(1.0, 0.55, s);
            c.rgb = mix(c.rgb, c.rgb * vec3(0.42, 0.04, 0.04) + vec3(0.05, 0.0, 0.0), m * 0.92);
        }
    }
#endif
    if ((draw.flags & DRAW_FOG) != 0u) {      // fog: towards the fog colour as the factor falls from 1
        vec3 fog = vec3(float(draw.fog_color & 255u), float((draw.fog_color >> 8) & 255u),
                        float((draw.fog_color >> 16) & 255u)) / 255.0;
        c.rgb = mix(fog, c.rgb, clamp(in_fog, 0.0, 1.0));
    }
    if ((draw.flags & DRAW_ALPHA_TEST) != 0u) {
        uint fn = draw.alpha_test & 7u, ref = (draw.alpha_test >> 8) & 255u, mask = (draw.alpha_test >> 16) & 255u;
        uint a = uint(c.a * 255.0 + 0.5) & mask;
        ref &= mask;
        bool pass = fn == 1u || (fn == 2u && a == ref) || (fn == 3u && a != ref) || (fn == 4u && a < ref) ||
                    (fn == 5u && a <= ref) || (fn == 6u && a > ref) || (fn == 7u && a >= ref);
        if (!pass)
            discard;
    }
    if ((draw.flags & DRAW_MOTION) != 0u) {  // drawn again into the motion blur's (post.c): its motion in the world, covered
        out_color = vec4(in_motion, 1.0);
        return;
    }
    out_color = c;
}
