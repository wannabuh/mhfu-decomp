// The GE's pixel stage before blending (ge.frag; ge_tl.frag, the GPU's vertex stage's, with TL_CORNERS - its pixels read
// their triangle's corners - and TL_WOUNDS; ge_tl_tess.frag, the same tessellated, with TL_WOUNDS alone): the texture
// function, colour doubling, the alpha test. Blending, depth and write masks are the fixed-function state set per draw.
#extension GL_EXT_nonuniform_qualifier : require
#include "shared.h"
layout(set = 0, binding = BIND_TEXTURES) uniform sampler2D textures[];
layout(set = 0, binding = BIND_SHADOW_MAP) uniform sampler2DShadow shadow_map;

// Lighting by the pixel: a draw's directional lights, in the world (direction to the light, w a power of the diffuse
// term or 0; colour), and which is the sun (the shadows')
layout(std430, set = 0, binding = BIND_LIGHTS) readonly buffer Lights { LightSet sets[]; };
layout(std430, set = 0, binding = BIND_BLOOD) readonly buffer Blood { uint blood_map[]; };
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
#endif
#ifdef TL_WOUNDS
layout(location = 14) flat in vec4 in_wound_look[4];
layout(location = 18) in float in_wound_facing[4];
layout(location = 22) flat in uint in_wound_tex;            // (the deep wounds' picture: its slot)
layout(location = 23) flat in vec4 in_wound_run[4];
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

float deep_of(float thick) { return smoothstep(0.25, 1.3, thick); }
// (the ground's blood's: a number that wanders across the ground, 0 to 1)
float blood_hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float blood_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(blood_hash(i), blood_hash(i + vec2(1.0, 0.0)), f.x), mix(blood_hash(i + vec2(0.0, 1.0)), blood_hash(i + vec2(1.0)), f.x), f.y);
}

// (the blood's: a number that wanders along x, 0 to 1)
float wound_blot(float x) {
    float i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(fract(sin(i * 127.1) * 43758.5453), fract(sin((i + 1.0) * 127.1) * 43758.5453), f);
}

#ifdef TL_WOUNDS
// (the wounds': a number that wanders along x, 0 to 1)
float wound_noise(float x) {
    float i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(fract(sin(i * 127.1) * 43758.5453), fract(sin((i + 1.0) * 127.1) * 43758.5453), f);
}
#endif

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
    // (draw.light[3][3]: how strong the stage's sun is, 0 to 1 - under a dull sky or in a cave its shade is that much
    // fainter, as is what is turned from it: render.c)
    float sun_strength = draw.light[3][3];
    float sh = (draw.flags & DRAW_WORLD) != 0u ? shadow() * sun_strength : 0.0;
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
            dark = 1.0 - (1.0 - sh) * mix(1.0, facing, sun_strength);
            // the relief, in the sun: lighter where it turns towards it, darker where away
            if ((draw.flags & DRAW_RELIEF) != 0u)
                relief = mix(1.0, clamp(dot(bumped, sun) / max(lambert, 0.25), 0.65, 1.35), 1.0 - dark);
        }
        c.rgb *= (1.0 - 0.45 * dark) * relief;
        // Blood on the ground (blood.c: a map of it, painted where drops land, moved while wet): the scenery's upward
        // faces stained by the cells about each pixel - those whose ground is as high as it. Its edge where enough
        // of it lies, uneven; thin blood a stain in the ground's own light, deep blood darker and its own colour;
        // wet blood redder, and shining where the sun is on it. Snow takes it up bright and wide, sand dark and dull.
        uint blood_info = BLOOD_SET_AT + uint(BLOOD_SETS) - 1u;
        if ((draw.flags & DRAW_WORLD) != 0u && sets[blood_info].count != 0u) {
            vec3 up = cross(dFdx(in_world), dFdy(in_world));
            float ul = length(up);
            vec3 eye = sets[blood_info].dir[0].xyz;
            vec2 from_eye = abs(in_world.xz - eye.xz);
            if (ul > 0.0 && abs(up.y) > 0.5 * ul && max(from_eye.x, from_eye.y) < float(BLOOD_N) * BLOOD_UNIT * 0.41) {
                vec2 g = in_world.xz / BLOOD_UNIT - 0.5;
                ivec2 g0 = ivec2(floor(g));
                vec2 f = fract(g);
                f = f * f * (3.0 - 2.0 * f);        // (rounder than straight between the cells: no corners)
                int here = int(floor(in_world.y / 8.0));
                float amount = 0.0, wet = 0.0, most = 0.0;
                uint which = 0u;
                for (int k = 0; k < 4; k++) {
                    ivec2 at = (g0 + ivec2(k & 1, k >> 1)) & (BLOOD_N - 1);
                    uint v = blood_map[at.y * BLOOD_N + at.x];
                    float a = float(v & 1023u);
                    int yc = int((v >> 18) & 1023u);
                    if (a == 0.0 || (yc != BLOOD_ANY && abs(((yc - here + 512) & 1023) - 512) > 4))
                        continue;
                    a *= ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
                    amount += a;
                    wet += a * float((v >> 10) & 255u);
                    if (a > most) {
                        most = a;
                        which = v >> 28;
                    }
                }
                if (amount > 0.0) {
                    wet /= amount * 255.0;
                    float thick = amount / 255.0;
                    uint ps = BLOOD_SET_AT + 2u * uint(BLOOD_DROP_SETS) + which / 8u;
                    vec3 col = (which & 4u) == 0u ? sets[ps].dir[which & 3u].rgb : sets[ps].color[which & 3u].rgb;
                    float ground = dot(c.rgb, vec3(0.3, 0.55, 0.15)), pale = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
                    float snow = smoothstep(0.5, 0.75, ground) * (1.0 - smoothstep(0.1, 0.25, pale));
                    float sand = smoothstep(1.2, 1.6, c.r / max(c.b, 0.05)) * smoothstep(0.25, 0.45, ground) * (1.0 - snow);
                    float n = blood_noise(in_world.xz * 0.9) * 0.6 + blood_noise(in_world.xz * 0.21) * 0.4;
                    float level = thick * (0.7 + 0.6 * n);
                    float edge = max(fwidth(level), 0.015);     // (a clean edge, a pixel wide)
                    float cover = smoothstep(0.2 - edge, 0.2 + edge, level);
                    // (how much of the ground shows through: little, less where it is deep - some patches thinner)
                    float body = mix(0.8, 0.98, deep_of(thick)) * (0.82 + 0.18 * smoothstep(0.25, 0.6, blood_noise(in_world.xz * 0.06 + 7.3)));
                    wet *= 1.0 - 0.6 * sand;
                    vec3 dry = mix(col * 0.6, vec3(0.3, 0.1, 0.07), 0.4);         // (dried: a dark red-brown, not black)
                    vec3 tint = mix(dry, col * 0.8, wet) * mix(1.0, 0.8, smoothstep(1.0, 3.0, thick));
                    tint = mix(tint, col * 1.15, 0.5 * snow);
                    tint = mix(tint, tint * 0.6, 0.6 * sand);
                    float deep = deep_of(thick);
                    vec3 stained = mix(c.rgb * (tint * 1.6 + 0.1), tint * (ground * 1.9 + 0.1), mix(0.7, 0.93, deep));
                    // (snow: a paler ring of it soaked out past the edge)
                    c.rgb = mix(c.rgb, c.rgb * mix(vec3(1.0), col * 1.3 + 0.25, 0.5), snow * smoothstep(0.02, 0.2, level) * (1.0 - cover));
                    c.rgb = mix(c.rgb, stained, cover * body * (1.0 - 0.25 * sand));
                    // (it shines: the sun on it and the sky, on a surface a little uneven - wet blood, the deeper the
                    // more; a little still when dry)
                    vec3 sunward = -vec3(draw.light[0][2], draw.light[1][2], draw.light[2][2]);
                    vec3 nrm = up * (up.y < 0.0 ? -1.0 : 1.0) / ul, view = normalize(eye - in_world);
                    nrm = normalize(nrm + 0.18 * vec3(blood_noise(in_world.xz * 0.5) - 0.5, 0.0, blood_noise(in_world.zx * 0.5 + 3.1) - 0.5));
                    float gloss = max(wet, 0.12) * mix(0.45, 1.0, deep) * cover * (1.0 - 0.8 * sand);
                    float sun_shine = dot(sunward, sunward) > 0.0 ? pow(max(dot(normalize(view + normalize(sunward)), nrm), 0.0), 45.0) * (1.0 - 0.85 * sh) : 0.0;
                    float sky_shine = pow(max(dot(nrm, normalize(view + vec3(0.0, 1.0, 0.0))), 0.0), 20.0) * 0.3 + 0.3 * pow(1.0 - max(dot(view, nrm), 0.0), 3.0);
                    c.rgb += (sun_shine * 0.9 + sky_shine * (0.25 + ground)) * gloss;
                }
            }
        }
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
#ifdef TL_WOUNDS
    // wounds (gpu.c): a cut where a blade landed, by how deep it went - a scratch: a thin line of the skin's own
    // colour darkened, hardly bleeding; a cut: a ragged line, dark inside, raw at its lips, a little blood about it;
    // a gash: wider and open, drawn from the wound's picture (render_texture.c) - or a shot's hole. Part of the skin, not laid on it: as light as the skin under it and no
    // lighter, its colours drawn towards the skin's, no sharper than the skin's own picture, its far lip lit and
    // its near one shaded by where the sun is, the skin about it a little darker. The blood dries, and in time the
    // wound fades.
    for (int k = 0; k < 4; k++) {
        vec3 q = in_wound[k];
        vec4 look = in_wound_look[k];
        // (its blood runs down from it, past the cut's own reach: where down was across the skin when it was made)
        vec4 run = in_wound_run[k];
        float steep = length(run.xy);
        bool cut_here = abs(q.x) < 1.0 && abs(q.y) < 1.0;
        float left = mod(look.y, 2.0), ran = floor(look.y * 0.5) / 1000.0;      // (how much of it is left; how far its blood has run)
        if (abs(q.z) >= 1.0 || left <= 0.0 || (!cut_here && steep < 0.1))
            continue;
        float seed = fract(look.w) * 64.0, kind = floor(look.w);
        bool hole = kind == 1.0;
        float wetness = floor(look.z) / 63.0, depth = fract(look.z) / 0.98;
        vec3 blood = vec3(mod(look.x, 256.0), mod(floor(look.x / 256.0), 256.0), floor(look.x / 65536.0)) / 255.0;
        float bleeds = smoothstep(0.15, 0.6, depth);      // (a scratch does not: the skin's colour, darker)
        // (the settings: how solid the blood that has run is - 0 as thin as it was - and how it shines)
        vec2 solid_shine = sets[BLOOD_SET_AT + uint(BLOOD_SETS) - 1u].dir[1].xy;
        // how soft its edges are: a pixel's worth of the wound, and no finer than the skin's picture is here
        float soft = clamp(fwidth(q.y) * 1.5 + 0.035, 0.035, 0.3);
        float gash = 0.0, stain = 0.0, v = 0.0, wide = 0.1;
        if (!cut_here) {
        } else if (!hole) {
            float taper = pow(max(1.0 - q.x * q.x, 0.0), 0.6);
            float mid = ((wound_noise(q.x * 2.5 + seed) - 0.5) * 0.45 + (wound_noise(q.x * 9.0 + seed * 3.0) - 0.5) * 0.14) * taper;
            wide = taper * (0.08 + 0.09 * wound_noise(q.x * 6.0 + seed * 5.0)) * mix(0.55, 1.1, depth);
            v = q.y - mid;
            gash = 1.0 - smoothstep(wide - soft, wide + soft, abs(v));
            float about = taper * (0.25 + 0.3 * wound_noise(q.x * 4.0 + seed * 9.0) + 0.12 * wound_noise(q.x * 23.0 + seed));
            stain = 1.0 - smoothstep(wide, max(about, wide + 0.01) + soft, abs(v));
        } else {
            float r = length(q.xy) * (1.0 + 0.5 * (wound_noise(atan(q.y, q.x) * 2.2 + seed) - 0.5));
            gash = 1.0 - smoothstep(0.2, 0.34, r);
            stain = 1.0 - smoothstep(0.3, 0.9, r);
        }
        // (not where the skin is turned from the cut: the far side of a limb, a fold lying by it)
        float m = (1.0 - smoothstep(0.6, 1.0, abs(q.z))) * left * smoothstep(0.1, 0.6, in_wound_facing[k]);
        vec3 skin = c.rgb;
        float lit = dot(skin, vec3(0.3, 0.55, 0.15));
        // the blood, drying; drawn towards the skin's colour, and as light as the skin is here
        vec3 dry = mix(blood * 0.55, vec3(0.16, 0.08, 0.05), 0.35);
        vec3 wet = mix(dry, blood, wetness * 0.7);
        vec3 tone = mix(wet, skin / max(lit, 0.02) * dot(wet, vec3(0.3, 0.55, 0.15)), 0.25);
        // its lips: the far one catches the light, the near one is in its own shade (which way across the cut the
        // sun lies, from the cut's slopes on the screen)
        vec3 over = dFdx(q.y) * dFdx(in_world) + dFdy(q.y) * dFdy(in_world);
        float ol = length(over);
        vec3 sunward = -normalize(vec3(draw.light[0][2], draw.light[1][2], draw.light[2][2]) + vec3(0.0, 1e-5, 0.0));
        float side = ol > 0.0 ? clamp(dot(over / ol, sunward) * sign(v) * 2.0, -1.0, 1.0) : 0.0;
        float lip_light = 1.0 + 0.3 * side * (hole ? 0.0 : 1.0);
        vec3 lips = mix(skin * 0.6, tone * lit * (2.5 + 0.6 * depth), bleeds) * lip_light;
        vec3 inside = mix(skin * 0.4, tone * lit * mix(0.75, 0.45, depth), bleeds);
        vec3 drawn = skin * (1.0 - 0.18 * stain * m * mix(0.3, 1.0, depth));          // the skin about it, darker
        drawn = mix(drawn, mix(drawn, tone * lit * 1.5, 0.6), stain * (0.22 + 0.14 * wetness) * m * bleeds * (0.5 + 0.7 * depth));
        drawn = mix(drawn, lips, gash * mix(0.6, 0.85, depth) * m);
        drawn = mix(drawn, inside, smoothstep(0.3, 0.85, gash) * mix(0.55, 0.9, depth) * m);
        // a deep cut from the picture: along it the cut, its alpha how much of the skin is covered; its colour as
        // painted where the blood is red, only its lightness where it is another colour - then as the rest: dried,
        // towards the skin's colour, lit as the skin. Read no sharper than the skin's picture
        float pictured = smoothstep(0.55, 0.8, depth) * (hole ? 0.0 : 1.0);
        if (cut_here && pictured > 0.0 && in_wound_tex != 0xFFFFFFFFu) {
            vec4 p = texture(textures[nonuniformEXT(in_wound_tex)], vec2(q.x, q.y * 0.8) * 0.5 + 0.5, 1.5);
            float light = max(p.r, max(p.g, p.b));
            bool red = blood.r > 2.0 * max(blood.g, blood.b);
            vec3 pc = red ? p.rgb : blood * light * 1.6;
            pc = mix(mix(pc * 0.6, vec3(0.16, 0.08, 0.05) * (light + 0.3), 0.3), pc, wetness * 0.7);
            pc = mix(pc, skin / max(lit, 0.02) * dot(pc, vec3(0.3, 0.55, 0.15)), 0.35) * lit * 2.2 * lip_light;
            drawn = mix(drawn, mix(skin * (1.0 - 0.18 * stain * m), pc, p.a * m * 0.9), pictured);
        }
        // The blood running down: from a cut that bleeds, on skin that slopes at all, runs from points along the cut
        // - each its own length and width and speed, wandering a little, a bead at its end - most of the way in a
        // few seconds, and staying where they got to. More of them, longer and wider, the deeper the cut.
        float runs = bleeds * smoothstep(0.25, 0.7, depth) * clamp(steep * 2.5, 0.0, 1.0) * (hole ? 0.5 : 1.0);
        float cover = 0.0;
        if (runs > 0.0 && steep >= 0.1) {
            vec2 on_skin = vec2(q.x * run.z, q.y * run.w), down = run.xy / steep, aside = vec2(-down.y, down.x);
            int many = depth > 0.65 ? 6 : 3;
            for (int r = 0; r < many; r++) {
                float pick = wound_noise(seed * 5.3 + float(r) * 17.0), quick = wound_noise(seed * 3.7 + float(r) * 5.0);
                vec2 from = vec2((pick * 2.0 - 1.0) * 0.8 * run.z, 0.0);
                float reach = 330.0 * runs * (0.3 + 0.7 * wound_noise(seed * 2.1 + float(r) * 9.0)) * pow(ran, 0.6 + 1.4 * quick);
                float along_run = dot(on_skin - from, down);
                if (along_run <= 0.0 || along_run >= reach || reach < 2.0)
                    continue;
                float off = dot(on_skin - from, aside) - (wound_noise(along_run * 0.05 + seed + float(r) * 3.0) - 0.5) * 11.0;
                float thick = mix(1.2, 3.0, depth) * (0.6 + 0.8 * quick) * (0.75 + 0.7 * smoothstep(0.8, 0.97, along_run / reach));
                cover = max(cover, (1.0 - smoothstep(thick * 0.4, thick + fwidth(off), abs(off))) *
                                   (1.0 - smoothstep(0.96, 1.0, along_run / reach)));
            }
            // (solid: its own colour and not the skin's under it, as light as the skin is here still)
            vec3 ran_tone = mix(wet, skin / max(lit, 0.02) * dot(wet, vec3(0.3, 0.55, 0.15)), 0.25 * (1.0 - solid_shine.x));
            drawn = mix(drawn, ran_tone * mix(lit * 1.9, max(lit * 1.9, 0.5 * (1.0 - 0.6 * sh)), solid_shine.x * 0.5),
                        cover * mix(0.8, 1.0, solid_shine.x) * m);
        }
        // Wet blood shines: the sun on it, and the sky - where the blood lies thick (in the cut, down its runs, a
        // little on the stain about it), as wet as it is, and a little still when it has dried
        float gloss = max(max(cover, smoothstep(0.3, 0.85, gash) * bleeds), 0.35 * stain * bleeds) * m * max(wetness, 0.12);
        if (gloss > 0.0) {
            vec3 n = cross(dFdx(in_world), dFdy(in_world));
            vec3 view = sets[BLOOD_SET_AT + uint(BLOOD_SETS) - 1u].dir[0].xyz - in_world;
            if (dot(n, n) > 0.0 && dot(view, view) > 0.0) {
                view = normalize(view);
                n = normalize(n);
                n *= dot(n, view) < 0.0 ? -1.0 : 1.0;
                float sun_shine = pow(max(dot(n, normalize(view + sunward)), 0.0), 40.0) * (1.0 - 0.85 * sh);
                float sky_shine = pow(max(dot(n, normalize(view + vec3(0.0, 1.0, 0.0))), 0.0), 18.0) * 0.3 + 0.25 * pow(1.0 - dot(n, view), 3.0);
                drawn += (sun_shine * 0.9 + sky_shine * (0.3 + lit)) * gloss * solid_shine.y;
            }
        }
        c.rgb = drawn;
    }
#endif
    // blood in the air (gpu.c: drops falling from wounds): each over what is behind it, in the world - where the line
    // from the eye to this pixel's place passes the drop, a streak as long as it is fast
    uint drops = (draw.flags & DRAW_KIND) != 0u && (draw.flags & DRAW_MOTION) == 0u ? sets[BLOOD_SET_AT].count : 0u;
    if (drops != 0u) {
        vec3 eye = sets[BLOOD_SET_AT + uint(BLOOD_SETS) - 1u].dir[0].xyz;
        vec3 ray = in_world - eye;
        float rr = dot(ray, ray);
        for (uint i = 0u; i < drops && i < 4u * uint(BLOOD_DROP_SETS) && rr > 0.0; i++) {
            uint s = BLOOD_SET_AT + i / 4u;
            vec4 at = sets[s].dir[i % 4u], col = sets[s].color[i % 4u];
            vec3 to = at.xyz - eye;
            float t = dot(to, ray) / rr;
            if (t <= 0.0 || t >= 1.0)
                continue;
            vec3 off = ray * t - to;                        // from the drop to the line's nearest point
            vec3 streak = sets[s + uint(BLOOD_DROP_SETS)].dir[i % 4u].xyz;
            streak -= ray * (dot(streak, ray) / rr);        // (as seen: across the line of sight)
            float ss = dot(streak, streak);
            if (ss > 0.0)
                off += streak * clamp(dot(off, -streak) / ss, 0.0, 1.0);
            float r = max(at.w, 0.0016 * length(to));      // (no thinner than a pixel or so, far off)
            c.rgb = mix(c.rgb, col.rgb * 0.42, 0.9 * (1.0 - smoothstep(r * 0.45, r, length(off))));
        }
    }
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
