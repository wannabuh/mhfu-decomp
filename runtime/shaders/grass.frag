#version 450
// Ground grass (grass.vert): the ground's colour, as the scenery is shaded - darker in the sun's shadow, and where the
// ground turns away from the sun - and the sun shining through the blades' thin tops when the camera looks towards
// it; the scenery's fog.
layout(set = 0, binding = 1) uniform sampler2DShadow shadow_map;
struct LightSet {
    vec4 dir[4];
    vec4 color[4];
    uint count, sun, pad0, pad1;
};
layout(std430, set = 0, binding = 2) readonly buffer Lights { LightSet sets[]; };
layout(push_constant) uniform Grass {
    mat4 light;
    uint camera, feet, nfeet, velocity;
} g;

layout(location = 0) in vec3 in_color;
layout(location = 1) in vec3 in_world;
layout(location = 2) in float in_t;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec3 in_view;
layout(location = 5) in float in_fog;
layout(location = 0) out vec4 out_color;

float shadow() {
    vec3 s = (g.light * vec4(in_world, 1.0)).xyz;
    float edge = max(abs(s.x), abs(s.y));
    if (edge >= 1.0 || s.z <= 0.0 || s.z >= 1.0)
        return 0.0;
    float lit = texture(shadow_map, vec3(s.xy * 0.5 + 0.5, s.z - 0.0008));
    return (1.0 - lit) * clamp((1.0 - edge) * 8.0, 0.0, 1.0);
}

void main() {
    vec3 c = in_color;
    float sh = shadow();
    vec3 sun = -normalize(vec3(g.light[0][2], g.light[1][2], g.light[2][2]));
    float facing = smoothstep(-0.1, 0.3, dot(in_normal, sun));
    float dark = 1.0 - (1.0 - sh) * facing;
    c *= 1.0 - 0.45 * dark;
    // the sun through the blades, looking towards it
    float towards = max(dot(normalize(in_view), sun), 0.0);
    c += c * (0.7 * towards * towards * towards * pow(in_t, 1.5) * (1.0 - dark));
    vec4 fog = sets[g.camera].color[3];
    c = mix(fog.rgb, c, in_fog);
    out_color = vec4(min(c, vec3(1.0)), 1.0);
}
