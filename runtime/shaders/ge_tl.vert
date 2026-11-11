#version 450
// The GE's vertex stage on the GPU (MHFU_GPU_TL): gpu.c's vertex() and light() for a draw (its TlDraw, the instance),
// from its vertices as the game gave them, decoded - to what ge.vert gives ge.frag, as render.c's put() would have
// it: the clip-space position of the render target, colours as bytes, the near plane where gpu.c clips.
#include "tl_common.glsl"

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) out float out_fog;
layout(location = 3) out vec3 out_shadow;
layout(location = 4) out vec3 out_sun;
layout(location = 5) out vec3 out_normal;
layout(location = 6) out vec3 out_world;
layout(location = 7) out vec4 out_shape;
layout(location = 8) out vec3 out_motion;
layout(location = 9) out vec3 out_corner;   // (the scenery's normal again, for ge_tl.frag's corners)
layout(location = 14) flat out vec4 out_wound_look[4];     // (how each looks: shared.h)
layout(location = 18) out float out_wound_facing[4];        // (how squarely the skin faces each: 1 as where it was cut)
layout(location = 22) flat out uint out_wound_tex;          // (the deep wounds' picture: its slot in the textures)
layout(location = 10) out vec3 out_wound[4];    // (each wound's: where the vertex is from it, 1 at its edge)
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[1];
};

// light(): the colour (0-255 a channel) and the sun's part of it (or the material's diffuse colour, the pixel lit)
void light(uint i, vec3 pos, vec3 normal, out vec4 color, out vec3 sun_part) {
    float len = length(normal);
    vec3 n = len > 0.0 ? normal / len : normal;
    uint flags = draws[i].flags;
    uint mode = draws[i].mode;
    vec3 mat_amb = draws[i].ambient.rgb, mat_dif = draws[i].diffuse.rgb, mat_spe = draws[i].specular.rgb;
    float mat_amb_a = draws[i].ambient.w;
    if ((mode & 1u) != 0u) {
        mat_amb = in_color.rgb;
        mat_amb_a = in_color.a;
    }
    if ((mode & 2u) != 0u)
        mat_dif = in_color.rgb;
    if ((mode & 4u) != 0u)
        mat_spe = in_color.rgb;
    float spow = draws[i].emissive.w;
    vec3 sum = draws[i].emissive.rgb + draws[i].global.rgb * mat_amb, spec = vec3(0.0), sun = vec3(0.0);
    int sun_light = int(draws[i].sun_light);
    for (int k = 0; k < 4; k++) {
        if ((flags & (TL_LIGHT_ON << k)) == 0u)
            continue;
        vec4 lp = draws[i].light_pos[k], ld4 = draws[i].light_dir[k], la4 = draws[i].light_att[k];
        int type = int(lp.w), comp = int(ld4.w);
        vec3 L = type == 0 ? lp.xyz : lp.xyz - pos;
        float d = length(L);
        if (d > 0.0)
            L /= d;
        float att = 1.0;
        if (type != 0) {
            float den = la4.x + la4.y * d + la4.z * d * d;
            att = den > 0.0 ? 1.0 / den : 1.0;
            att = min(att, 1.0);
        }
        if (type == 2) {                // spot: the cone around the light's direction
            float dl = length(ld4.xyz);
            float cosang = dl > 0.0 ? -dot(L, ld4.xyz) / dl : 0.0;
            att *= cosang >= la4.w ? powc(max(cosang, 0.0), draws[i].light_ambient[k].w) : 0.0;
        }
        vec3 la = draws[i].light_ambient[k].rgb, ld = draws[i].light_diffuse[k].rgb;
        vec3 ls = draws[i].light_specular[k].rgb;
        float dt = max(dot(n, L), 0.0);
        if (comp == 2)
            dt = powc(dt, spow);
        bool gpu = (flags & (TL_LIGHT_PIXEL << k)) != 0u;  // its diffuse term by the pixel
        sum += (la * mat_amb + (gpu ? vec3(0.0) : ld * mat_dif * dt)) * att;
        if (k == sun_light && !gpu)
            sun += ld * mat_dif * dt * att;
        if (comp == 1 && dt > 0.0) {    // specular: the half vector with the viewer straight ahead
            vec3 h = vec3(L.xy, L.z + 1.0);
            float hl = length(h);
            float sd = hl > 0.0 ? dot(n, h) / hl : 0.0;
            float sp = sd > 0.0 ? pow(sd, spow) : 0.0;
            spec += ls * mat_spe * sp * att;
            if (k == sun_light)
                sun += ls * mat_spe * sp * att;
        }
    }
    vec3 rgb = clamp(sum + spec, 0.0, 1.0);
    sun = rgb - max(rgb - sun, 0.0);    // the sun's part: what the colour loses without it (as clamped)
    sun_part = (flags & (15u * TL_LIGHT_PIXEL)) != 0u ? mat_dif * 255.0 : sun * 255.0;
    color = vec4(rgb * 255.0, mat_amb_a * draws[i].global.w * 255.0);
}

void main() {
    uint i = uint(gl_InstanceIndex);
    uint flags = draws[i].flags;
    vec3 p = in_pos;
    vec3 nr = (flags & TL_FLIP) != 0u ? -in_normal : in_normal;
    vec3 shape = (flags & TL_FLIP) != 0u ? -in_shape : in_shape, unused = p;
    vec3 ew = vec3(0.0);
    if ((flags & TL_EARLIER) != 0u) {   // where it was a frame shown earlier
        vec3 q = p, qn = vec3(0.0);
        tl_skin(i, true, q, qn);
        ew = tl_world(i, xf(draws[i].earlier_world[0], draws[i].earlier_world[1], draws[i].earlier_world[2], q));
    }
    tl_skin(i, false, unused, shape);   // (the shape normal: as the normal)
    tl_skin(i, false, p, nr);           // skinning: the weighted sum of the bones' transforms
    vec3 still = xf(draws[i].world[0], draws[i].world[1], draws[i].world[2], p), m = tl_sway(i, still);
    vec3 world = vec3(0.0), motion = vec3(0.0);
    if ((flags & TL_WORLD) != 0u) {
        world = tl_world(i, m);
        if ((flags & TL_EARLIER) != 0u) // (the wind's move not in it, as the CPU's)
            motion = tl_world(i, still) - ew;
    }
    vec4 color = in_color;
    vec3 sun = vec3(0.0), normal = vec3(0.0);
    if ((flags & TL_LIT) != 0u) {       // lighting, in the GE's world space
        vec3 wn = xf3(draws[i].world[0], draws[i].world[1], draws[i].world[2], nr);
        vec4 c;
        light(i, m, wn, c, sun);
        color = floor(clamp(c, 0.0, 255.0)) / 255.0;    // (as put() makes bytes of it)
        if ((flags & (15u * TL_LIGHT_PIXEL)) != 0u) {
            normal = xf3(draws[i].to_world[0], draws[i].to_world[1], draws[i].to_world[2], wn);
            float nl = length(normal);
            normal = nl > 0.0 ? normal / nl : vec3(0.0);
        }
    }
    vec3 e = xf(draws[i].view[0], draws[i].view[1], draws[i].view[2], m);
    // fog: (end + view-space depth) * slope, 1 for none and 0 for all fog
    float fog = (draws[i].offset.z + e.z) * draws[i].offset.w;
    out_fog = isnan(fog) ? 1.0 : clamp(fog, 0.0, 1.0);
    vec4 clip = draws[i].proj * vec4(e, 1.0);
    // to the render target as put() places it: the viewport and screen offset in PSP pixels, over half the target
    // (linear in the clip space: x / w * scale + offset, over 256, less one - times w); the depth clamped as put()'s
    vec4 vp = draws[i].viewport, off = draws[i].offset;
    float w = clip.w;
    float z = w > 0.0 ? clamp((clip.z / w * vp.z + vp.w) / 65535.0, 0.0, 1.0) : 0.0;
    gl_Position = vec4(clip.x * (vp.x / (RT_HALF)) + w * (off.x / (RT_HALF) - 1.0),
                       clip.y * (vp.y / (RT_HALF)) + w * (off.y / (RT_HALF) - 1.0), z * w, w);
    gl_ClipDistance[0] = w - 1e-4;      // (gpu.c's near plane)
    // (the world: this frame's offset for the anti-aliasing, a fraction of a pixel - pad0, pad1: its clip-space x, y)
    gl_Position.xy += uintBitsToFloat(uvec2(draw.pad0, draw.pad1)) * w;
    out_uv = tl_uv(i);
    out_color = color;
    out_shadow = (draw.light * vec4(world, 1.0)).xyz;
    out_sun = floor(clamp(sun, 0.0, 255.0)) / 255.0;
    // (the scenery shaded by the sun: its normal, made from its triangles - render.c tl_scenery)
    out_normal = (draw.flags & DRAW_SHADED) != 0u ? extra[2u * TL_WIND + uint(gl_VertexIndex)].xyz : normal;
    out_corner = out_normal;
    out_world = world;
    // (wounds: among the vertices as the game gave them, so they stay on the skin however it is posed. Linear in the
    // place, so the pixels' are exact: the way across over the half width, the way along over the half length)
    out_wound_tex = draws[i].wounds != 0u ? draws[i].wound_pad0 : 0xFFFFFFFFu;
    for (uint k = 0u; k < 4u; k++) {
        if (k < draws[i].wounds) {
            vec4 r0 = draws[i].wound[3u * k], r1 = draws[i].wound[3u * k + 1u], r2 = draws[i].wound[3u * k + 2u];
            vec3 p = in_pos - vec3(r0.w, r1.w, r2.w);
            out_wound[k] = vec3(dot(r0.xyz, p), dot(r1.xyz, p), dot(r2.xyz, p));
            out_wound_look[k] = draws[i].wound_look[k];
            // (the skin's normal against the wound's, both as the game gave the vertices: the other side of a
            // limb, a fold beside it are not cut - through the rows' inverse transpose, near enough: the rows)
            vec3 wn = vec3(dot(r0.xyz, in_normal), dot(r1.xyz, in_normal), dot(r2.xyz, in_normal));
            float wl = length(wn);
            out_wound_facing[k] = wl > 0.0 ? wn.z / wl : 1.0;
        } else {
            out_wound[k] = vec3(10.0);
            out_wound_look[k] = vec4(0.0);
            out_wound_facing[k] = 0.0;
        }
    }
    // (tessellated: the shape normal in the world as the normal is, and its triangle's straight edges)
    vec3 sw = xf3(draws[i].to_world[0], draws[i].to_world[1], draws[i].to_world[2],
                  xf3(draws[i].world[0], draws[i].world[1], draws[i].world[2], shape));
    float sl = length(sw);
    out_shape = vec4(sl > 0.0 ? sw / sl : vec3(0.0), float(in_edges));
    out_motion = motion;
}
