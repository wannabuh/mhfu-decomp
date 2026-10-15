#version 450
// Ground grass (grass.c): each instance a blade, its 18 vertices a strip of three quads from the root to the tip
// (gl_VertexIndex: quad v / 6, its corner v % 6) - tapering, arching over its own way, bent by the wind (waves of
// different lengths crossing the field, gusts passing over it, each blade swaying in its own measure) and pressed down
// where it is trodden (grass.c's trail: smooth, the way the feet went, springing back), turned to face the camera. Towards the field's edge it thins out (by its place in its tile's shuffled order) and shrinks away;
// a blade thinner than a pixel is drawn a pixel wide (it would flicker).
layout(location = 0) in vec3 in_root;
layout(location = 1) in uvec4 in_data;      // colour, shape, normal, (none)

struct LightSet {
    vec4 dir[4];
    vec4 color[4];
    uint count, sun, pad0, pad1;
};
layout(std430, set = 0, binding = 2) readonly buffer Lights { LightSet sets[]; };

// The trodden grass (grass.c grass_trail): a window of 256 x 256 cells of 6 units, wrapping, each the way and how far
// it is pressed (snorm16 x, z); this frame's from trail on
layout(std430, set = 0, binding = 4) readonly buffer Trail { uint trail_cells[]; };

// camera: the set holding the world to the clip space (dir: its columns), the eye (color[0]), the time, the wind's
// strength, render target pixels a clip unit, the field's reach (color[1]), the fog's end, slope, on (color[2]) and
// colour (color[3]); trail, trail_x, trail_z: this frame's trodden grass, its window's first cell
layout(push_constant) uniform Grass {
    mat4 light;
    uint camera, trail;
    int trail_x, trail_z;
} g;

const int TRAIL = 256;
const float TRAIL_CELL = 6.0;

vec2 trodden(vec2 xz) {
    vec2 c = xz / TRAIL_CELL - 0.5;
    ivec2 b = ivec2(floor(c));
    vec2 f = c - vec2(b);
    vec2 sum = vec2(0.0);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            ivec2 q = b + ivec2(i, j);
            if (q.x < g.trail_x || q.y < g.trail_z || q.x >= g.trail_x + TRAIL || q.y >= g.trail_z + TRAIL)
                continue;
            float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            sum += w * unpackSnorm2x16(trail_cells[g.trail + uint((q.y & (TRAIL - 1)) * TRAIL + (q.x & (TRAIL - 1)))]);
        }
    return sum;
}

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// smooth value noise, 0..1
float noise(vec2 p) {
    vec2 i = floor(p), f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0)), f.x), f.y);
}

layout(location = 0) out vec3 out_color;
layout(location = 1) out vec3 out_world;
layout(location = 2) out float out_t;
layout(location = 3) out vec3 out_normal;
layout(location = 4) out vec3 out_view;
layout(location = 5) out float out_fog;

const float T[4] = float[4](0.0, 0.4, 0.75, 1.0);
const float W[4] = float[4](1.0, 0.82, 0.48, 0.0);
const int LEVEL[6] = int[6](0, 0, 1, 0, 1, 1);
const float SIDE[6] = float[6](-1.0, 1.0, -1.0, 1.0, 1.0, -1.0);

// a sideways bend b of a point above units up its blade, keeping its length: the point comes down as it goes over
vec3 keep_length(vec3 b, float above) {
    float l = length(b), most = 0.9 * above;
    if (l > most)
        b *= most / max(l, 1e-4);
    b.y = -(above - sqrt(max(above * above - dot(b.xz, b.xz), 0.0)));
    return b;
}

void main() {
    LightSet cam = sets[g.camera];
    mat4 to_clip = mat4(cam.dir[0], cam.dir[1], cam.dir[2], cam.dir[3]);
    vec3 eye = cam.color[0].xyz;
    float time = cam.color[1].x, wind = cam.color[1].y, px = cam.color[1].z, reach = cam.color[1].w;
    int level = gl_VertexIndex / 6 + LEVEL[gl_VertexIndex % 6];
    float side = SIDE[gl_VertexIndex % 6], t = T[level];

    vec3 root = in_root;
    uint shape = in_data.y;
    float height = float(shape & 255u) * 0.25, half_w = float((shape >> 8) & 255u) / 64.0;
    float yaw = float((shape >> 16) & 255u) * (6.2831853 / 256.0), rank = float(shape >> 24) / 255.0;
    vec2 nxz = vec2(float(int(in_data.z << 24) >> 24), float(int(in_data.z << 16) >> 24)) / 127.0;
    vec3 n = vec3(nxz.x, sqrt(max(1.0 - dot(nxz, nxz), 0.0)), nxz.y);
    float phase = float((in_data.z >> 16) & 255u) * (6.2831853 / 256.0);

    // the distance: thinned by the order (a blade goes when the density there - full near, then as the square of the
    // near distance over it - falls below its place), the rest wider to keep the field's look; at the reach shrinking
    // away
    float dist = length(root.xz - eye.xz), near = cam.color[0].w;
    float keep = dist < near ? 1.0 : (near / dist) * (near / dist);
    float fade = clamp((keep - rank) / max(0.06 * keep, 0.004), 0.0, 1.0) * (1.0 - smoothstep(0.85 * reach, reach, dist));
    half_w *= min(inversesqrt(max(keep, 1e-3)), 3.0);
    float h = height * fade;

    vec3 up = normalize(vec3(0.0, 1.0, 0.0) * 0.85 + n * 0.15);
    vec3 rr = vec3(cos(yaw), 0.0, sin(yaw));
    vec3 axis = root + up * (h * t) + rr * (0.28 * h * t * t);
    // the wind: its own measure for each blade (how strongly, how fast it flutters, a little off the wind's way), waves
    // of three lengths crossing the field at angles, and gusts - patches of stronger wind - drifting over it
    float r1 = fract(phase * 0.1591549 * 7.31), r2 = fract(phase * 0.1591549 * 13.7 + 0.37);
    vec2 wd = normalize(vec2(0.8, 0.6));
    vec2 cw = vec2(-wd.y, wd.x);
    vec2 xz = root.xz;
    float w1 = sin(dot(xz, wd) * 0.021 - time * 2.1 + phase * 0.45);
    float w2 = sin(dot(xz, normalize(wd + 0.6 * cw)) * 0.037 - time * 2.9 + 1.7);
    float w3 = sin(dot(xz, normalize(wd - 0.8 * cw)) * 0.061 - time * 3.7 + 4.1 + phase * 0.3);
    float gust = smoothstep(0.45, 0.85, noise(xz * 0.0022 - wd * time * 0.55 + vec2(3.7, 1.3)));
    float flutter = sin(time * (4.0 + 3.0 * r1) + phase) * (0.1 + 0.12 * r2) + sin(time * (7.3 + 2.0 * r2) + 2.0 * phase) * 0.05;
    float sway = (0.35 + 0.22 * w1 + 0.14 * w2 + 0.08 * w3) * (0.7 + 0.6 * r1) + gust * (0.55 + 0.25 * w3) + flutter;
    vec2 way = normalize(wd + cw * ((r2 - 0.5) * 0.7 + 0.25 * sin(time * 1.3 + phase)));
    vec2 bend = way * (0.3 * h * t * t * sway * wind);
    // trodden: pressed the way the feet went, the top most
    vec2 push = trodden(root.xz);
    // (laid over quickly, two thirds of the way down: the length kept bends it down as it goes over)
    float pl = length(push), lay = smoothstep(0.0, 0.5, pl);
    vec2 lay_dir = pl > 1e-3 ? push / pl : vec2(0.0);
    bend += lay_dir * (lay * 0.65 * h * t * sqrt(t));
    axis += keep_length(vec3(bend.x, 0.0, bend.y), h * t);

    // across the view, square to the blade
    vec3 view = axis - eye;
    vec3 dir = normalize(up + rr * (0.56 * t) + vec3(bend.x, 0.0, bend.y) / max(h, 1e-3));
    vec3 right = cross(dir, view);
    float rl = length(right);
    right = rl > 1e-4 ? right / rl : vec3(1.0, 0.0, 0.0);
    vec4 rc = to_clip * vec4(root, 1.0);
    float per_unit = length(vec3(to_clip[0][1], to_clip[1][1], to_clip[2][1])) * px / max(rc.w, 1e-3);
    float widen = max(1.0, 0.9 / max(2.0 * half_w * per_unit, 1e-4));
    vec3 p = axis + right * (side * half_w * W[level] * widen * max(fade, 0.3));
    gl_Position = to_clip * vec4(p, 1.0);

    out_color = vec3(float(in_data.x & 255u), float((in_data.x >> 8) & 255u), float((in_data.x >> 16) & 255u)) / 255.0 *
                mix(0.78, 1.16, t);
    // trodden: crushed blades a darker, deeper green, in the shade of the grass standing about them; and as a lawn's
    // stripes, those lying away from the camera showing their lighter backs, those lying towards it their shaded
    // faces - so a trail shows whichever way it is seen (end-on, laid blades are hardly told from standing ones)
    if (lay > 0.0) {
        vec2 away = root.xz - eye.xz;
        float facing = dot(lay_dir, away / max(length(away), 1e-3));
        out_color *= mix(vec3(1.0), vec3(0.78, 0.86, 0.74), lay) * (1.0 + lay * (0.2 * facing - 0.1) * (0.5 + 0.5 * t));
    }
    out_world = p;
    out_t = t;
    out_normal = n;
    out_view = normalize(view);
    vec4 fog = cam.color[2];
    out_fog = fog.z > 0.5 ? clamp((fog.x - gl_Position.w) * fog.y, 0.0, 1.0) : 1.0;
}
