// Motion blur (post.c), what its passes share: a pixel's motion on the screen as far as the shutter takes it. Where
// render_post.c drew the hunter, monsters and people into the motion image (source2), their own motion in the world since
// the frame shown before; everywhere else, the camera's alone. Their place then, through the last frame's camera
// (to_shadow: the camera's space to its PSP pixels); the sky, a direction, by its turning.
#include "post_common.glsl"

// (pc.sun, sun_color, bloom: the world to the camera's space, its rows; texel.x: the shown frames in the shutter's
// time, y: the longest smear in pixels, z: the tiles' size in pixels)
vec2 motion(ivec2 pix) {
    float d = texelFetch(depth, pix, 0).r;
    vec3 p = view_pos(pix, d > 0.0 ? d : 1.0);
    vec4 q;
    if (d > 0.0) {
        vec4 own = texelFetch(source2, pix, 0);
        if (own.a > 0.5)
            p -= vec3(dot(pc.sun.xyz, own.xyz), dot(pc.sun_color.xyz, own.xyz), dot(pc.bloom.xyz, own.xyz));
        q = pc.to_shadow * vec4(p, 1.0);
    } else
        q = pc.to_shadow * vec4(normalize(p), 0.0);
    if (q.w <= 1e-6)
        return vec2(0.0);
    vec2 m = ((vec2(pix) + 0.5) / pc.area.z - q.xy / q.w) * pc.area.z * pc.texel.x;
    float len = length(m);
    return len > pc.texel.y ? m * (pc.texel.y / len) : m;
}
