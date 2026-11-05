#version 450
// Temporal anti-aliasing (post.c), the first of its passes: the world is drawn a fraction of a pixel off each frame
// shown (eight places in turn), and each pixel blended into what it showed before - where that point was then,
// through the last frame's camera and the hunter's, monsters' and people's own motion (as the motion blur finds it).
// What it showed before is kept to the colours about the pixel now (a box in YCoCg, by their spread), so what has
// moved or changed leaves no ghost; over a few frames edges, grass and texture detail settle instead of shimmering.
#include "post_common.glsl"
layout(location = 0) out vec4 out_color;

// (source: the picture; source2: the motion image; source3: the last frame's result. to_shadow: the camera's space
// to the last frame's PSP pixels; sun, sun_color, bloom: the world to the camera's space, its rows. texel.xy: this
// frame's offset in pixels, z: the share of the new frame, w: 1 if there is a last frame's result)

vec3 to_ycocg(vec3 c) {
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 from_ycocg(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The last frame's result at a point (pixels), through a Catmull-Rom kernel in nine bilinear taps (as sharp as
// it was: bilinear alone would soften it a little more each frame)
vec3 history(vec2 at) {
    vec2 size = pc.area.xy, p = at - 0.5, f = fract(p), base = floor(p) + 0.5;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2, t0 = (base - 1.0) / size, t3 = (base + 2.0) / size, t12 = (base + w2 / w12) / size;
    vec3 c = (texture(source3, vec2(t0.x, t0.y)).rgb * w0.x + texture(source3, vec2(t12.x, t0.y)).rgb * w12.x +
              texture(source3, vec2(t3.x, t0.y)).rgb * w3.x) * w0.y +
             (texture(source3, vec2(t0.x, t12.y)).rgb * w0.x + texture(source3, vec2(t12.x, t12.y)).rgb * w12.x +
              texture(source3, vec2(t3.x, t12.y)).rgb * w3.x) * w12.y +
             (texture(source3, vec2(t0.x, t3.y)).rgb * w0.x + texture(source3, vec2(t12.x, t3.y)).rgb * w12.x +
              texture(source3, vec2(t3.x, t3.y)).rgb * w3.x) * w3.y;
    float total = (w0.x + w12.x + w3.x) * (w0.y + w12.y + w3.y);
    return max(c / total, vec3(0.0));
}

void main() {
    ivec2 x = ivec2(gl_FragCoord.xy), last = ivec2(pc.area.xy) - 1;
    vec4 now = texelFetch(source, x, 0);
    if (pc.texel.w < 0.5) {
        out_color = now;
        return;
    }
    // the colours about it, and the nearest point among them (its motion is the one edges move with)
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    float nearest = -1.0;
    ivec2 at = x;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            ivec2 p = clamp(x + ivec2(dx, dy), ivec2(0), last);
            vec3 c = to_ycocg(texelFetch(source, p, 0).rgb);
            m1 += c;
            m2 += c * c;
            float d = texelFetch(depth, p, 0).r;
            if (d > nearest) {
                nearest = d;
                at = p;
            }
        }
    vec3 mean = m1 / 9.0, spread = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    vec3 lo = mean - 1.25 * spread, hi = mean + 1.25 * spread;
    // where it was: this point (without this frame's offset) in the camera's space, less its own motion, through
    // the last frame's camera
    vec2 centre = vec2(x) + 0.5 - pc.texel.xy;
    vec4 v = pc.to_view * vec4(centre / pc.area.z, nearest > 0.0 ? nearest : 1.0, 1.0);
    vec3 p = v.xyz / v.w;
    vec4 q;
    vec4 own = vec4(0.0);
    if (nearest > 0.0) {
        own = texelFetch(source2, at, 0);
        if (own.a > 0.5)
            p -= vec3(dot(pc.sun.xyz, own.xyz), dot(pc.sun_color.xyz, own.xyz), dot(pc.bloom.xyz, own.xyz));
        q = pc.to_shadow * vec4(p, 1.0);
    } else
        q = pc.to_shadow * vec4(normalize(p), 0.0);
    vec2 then = q.xy / q.w * pc.area.z;
    if (q.w <= 1e-6 || any(lessThan(then, vec2(0.0))) || any(greaterThan(then, pc.area.xy))) {
        out_color = now;
        return;
    }
    vec3 before = to_ycocg(history(then));
    if (pc.area.w == 4.0) {             // (4: the same, the pixel where it is now)
        out_color = vec4(abs(texelFetch(source3, x, 0).rgb - now.rgb) * 4.0, now.a);
        return;
    }
    if (pc.area.w == 5.0) {             // (5: where it was, as an offset: red right, green down, 1 a pixel ... 0.5 grey)
        out_color = vec4(clamp((then - vec2(x) - 0.5) * 0.05 + 0.5, 0.0, 1.0), 0.5, now.a);
        return;
    }
    if (pc.area.w == 3.0) {             // (MHFU_POST_DEBUG=3: how far the last frame's, brought here, is from now)
        out_color = vec4(abs(from_ycocg(before) - now.rgb) * 4.0, now.a);
        return;
    }
    // What moves by itself (the hunter, monsters, people) at 2 pixels a frame or more: its surface changes as it bends and
    // turns and history resampled that far softens it, so the history is held closer and less of it kept. The rest,
    // and all of it standing still, as above
    float k = own.a > 0.5 ? clamp(length(then - vec2(x) - 0.5) / 2.0, 0.0, 1.0) : 0.0;
    float gamma = mix(1.25, 0.75, k), share = mix(pc.texel.z, max(pc.texel.z, 0.4), k);
    lo = mean - gamma * spread;
    hi = mean + gamma * spread;
    vec3 raw = before;
    before = clamp(before, lo, hi);
    vec3 result = mix(before, to_ycocg(now.rgb), share);
    if (pc.area.w == 8.0) {             // (8: red, how far the last frame's brought here is from now, x4; green: moves by itself; blue:
                                        // how far the result is from the last frame's, x8; luma)
        vec3 nowc = to_ycocg(now.rgb);
        out_color = vec4(abs(raw.x - nowc.x) * 4.0, texelFetch(source2, x, 0).a, abs(result.x - raw.x) * 8.0, now.a);
        return;
    }
    out_color = vec4(max(from_ycocg(result), vec3(0.0)), now.a);
}
