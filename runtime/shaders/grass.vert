#version 450
// Ground grass (grass.c): each instance a blade, its 18 vertices a strip of three quads from the root to the tip
// (gl_VertexIndex: quad v / 6, its corner v % 6) - tapering, arching over its own way, bent by the wind (the plants'
// wind: waves running through the field, each blade's flutter) and away from the feet walking through it, turned to
// face the camera. Towards the field's edge it thins out (by its place in its tile's shuffled order) and shrinks away;
// a blade thinner than a pixel is drawn a pixel wide (it would flicker).
layout(location = 0) in vec3 in_root;
layout(location = 1) in uvec4 in_data;      // colour, shape, normal, (none)

struct LightSet {
    vec4 dir[4];
    vec4 color[4];
    uint count, sun, pad0, pad1;
};
layout(std430, set = 0, binding = 2) readonly buffer Lights { LightSet sets[]; };

// camera: the set holding the world to the clip space (dir: its columns), the eye (color[0]), the time, the wind's
// strength, render target pixels a clip unit, the field's reach (color[1]), the fog's end, slope, on (color[2]) and
// colour (color[3]); feet: the set holding the feet (x, y, z, radius: dir then color), nfeet of them
layout(push_constant) uniform Grass {
    mat4 light;
    uint camera, feet, nfeet;
} g;

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

    // the field's edge: thinned by the order (a blade goes when the density there falls below its place), then
    // shrinking away
    float dist = length(root.xz - eye.xz);
    float keep = dist < 0.3 * reach ? 1.0 : 1.0 - 0.75 * (dist - 0.3 * reach) / (0.7 * reach);
    float fade = clamp((keep - rank) / 0.06, 0.0, 1.0) * (1.0 - smoothstep(0.85 * reach, reach, dist));
    float h = height * fade;

    vec3 up = normalize(vec3(0.0, 1.0, 0.0) * 0.85 + n * 0.15);
    vec3 rr = vec3(cos(yaw), 0.0, sin(yaw));
    vec3 axis = root + up * (h * t) + rr * (0.28 * h * t * t);
    // the wind
    vec2 wd = normalize(vec2(0.8, 0.6));
    float along = dot(root.xz, wd), across = dot(root.xz, vec2(-wd.y, wd.x));
    float sway = 0.45 + 0.35 * sin(along * 0.012 - time * 2.4 + 0.7 * sin(phase)) +
                 0.15 * sin(across * 0.006 + along * 0.008 - time * 1.7 + 1.3) + 0.12 * sin(time * 5.3 + phase * 1.7);
    vec2 bend = wd * (0.3 * h * t * t * sway * wind);
    // the feet walking through: bent away from each, the top most
    vec2 push = vec2(0.0);
    for (uint i = 0u; i < g.nfeet; i++) {
        vec4 f = i < 4u ? sets[g.feet].dir[i] : sets[g.feet].color[i - 4u];
        if (abs(root.y - f.y) > 60.0)
            continue;
        vec2 d = root.xz - f.xz;
        float l = length(d), r = 30.0 + f.w;
        if (l >= r)
            continue;
        push += (l > 1e-3 ? d / l : vec2(0.7071)) * (1.0 - smoothstep(0.2 * r, r, l));
    }
    float pl = length(push);
    if (pl > 1.0)
        push /= pl;
    bend += push * (0.85 * h * t * sqrt(t));
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
    out_world = p;
    out_t = t;
    out_normal = n;
    out_view = normalize(view);
    vec4 fog = cam.color[2];
    out_fog = fog.z > 0.5 ? clamp((fog.x - gl_Position.w) * fog.y, 0.0, 1.0) : 1.0;
}
