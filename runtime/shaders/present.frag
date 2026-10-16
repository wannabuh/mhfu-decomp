#version 450
// The picture onto the window. Its own size there: each pixel the picture's own, untouched. Otherwise resampled
// through a Catmull-Rom kernel, widened by the shrink when the picture is larger than the window (supersampling: all
// its pixels count, so its extra resolution smooths edges rather than being skipped), sharp either way - one bilinear
// tap a pixel blurred it, half a pixel off at almost the same size
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Present {
    vec2 source;        // the picture's size in the render target, texels (from its corner)
    vec2 origin;        // where it goes in the window, and its size there, pixels
    vec2 size;
} pc;
layout(location = 0) out vec4 out_color;

float catmull_rom(float x) {
    x = abs(x);
    return x < 1.0 ? (1.5 * x - 2.5) * x * x + 1.0 : x < 2.0 ? ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0 : 0.0;
}

void main() {
    vec2 p = gl_FragCoord.xy - pc.origin;
    ivec2 last = ivec2(pc.source) - 1;
    if (pc.source == pc.size) {
        out_color = vec4(texelFetch(source, clamp(ivec2(p), ivec2(0), last), 0).rgb, 1.0);
        return;
    }
    vec2 ratio = pc.source / pc.size;
    vec2 c = p * ratio;                                 // this pixel's centre in the picture, texels
    vec2 k = max(ratio, vec2(1.0));                     // the kernel's width, texels to its unit
    // shrunk more than twice: taps of 2 x 2 texels (bilinear at their shared corner), so at most 10 a side
    int step = max(k.x, k.y) > 2.0 ? 2 : 1;
    k = min(k, vec2(4.5));
    ivec2 lo = ivec2(floor((c - 2.0 * k) / float(step))), hi = ivec2(ceil((c + 2.0 * k) / float(step)));
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int y = lo.y; y <= hi.y; y++) {
        float cy = (float(y) + 0.5) * float(step);      // the tap's centre
        float wy = catmull_rom((cy - c.y) / k.y);
        if (wy == 0.0)
            continue;
        for (int x = lo.x; x <= hi.x; x++) {
            float cx = (float(x) + 0.5) * float(step);
            float w = catmull_rom((cx - c.x) / k.x) * wy;
            if (w == 0.0)
                continue;
            vec3 s;
            if (step == 1)
                s = texelFetch(source, clamp(ivec2(x, y), ivec2(0), last), 0).rgb;
            else
                s = texture(source, clamp(vec2(cx, cy), vec2(1.0), vec2(last)) / vec2(textureSize(source, 0))).rgb;
            sum += s * w;
            total += w;
        }
    }
    out_color = vec4(clamp(sum / total, 0.0, 1.0), 1.0);
}
