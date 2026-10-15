// Post-processing (post.c): what its passes share - their inputs, and the frame's camera, sun and settings.
layout(set = 0, binding = 0) uniform sampler2D source;          // the pass's input
layout(set = 0, binding = 1) uniform sampler2D source2;         // a second (the glow; the bloom)
layout(set = 0, binding = 2) uniform sampler2D depth;           // the picture's depth (full resolution)
layout(set = 0, binding = 3) uniform sampler2DShadow shadow_map; // the sun's
layout(set = 0, binding = 4) uniform sampler2D source3;         // a third (the light shafts)
layout(set = 0, binding = 5) uniform sampler2D source4;         // a fourth (the depth of field's blur)

layout(push_constant) uniform Post {
    mat4 to_view;       // (PSP pixel x, y, depth, 1) to the camera's space (world units)
    mat4 to_shadow;     // the camera's space to the shadow map
    vec4 texel;         // the source's texel size (x, y); the first bloom pass (z)
    vec4 area;          // the picture's size in pixels (x, y), render target pixels a PSP pixel (z)
    vec4 sun;           // the direction to the sun (camera space), the shafts' strength (w; 0: none)
    vec4 sun_color;     // its colour, the shafts' contrast (w)
    vec4 bloom;         // strength, the threshold of the picture's own light, how much of it, the glow's gain
    vec4 grade;         // saturation, contrast, warmth, vignette
    vec4 dof;           // the focus distance, the share of it beyond which the blur begins, the largest blur (half
                        // resolution pixels; 0: none)
} pc;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// A pixel of the picture (full resolution) in the camera's space; depth 0 is the sky (nothing drawn there)
vec3 view_pos(ivec2 pix, float d) {
    vec4 v = pc.to_view * vec4((vec2(pix) + 0.5) / pc.area.z, d, 1.0);
    return v.xyz / v.w;
}

// Depth of field: how blurred a point this far is (half resolution pixels) - only what is beyond the focus (the thin
// lens' 1 - focus / distance, from the in-focus band on)
float coc(float dist) {
    float t = 1.0 - pc.dof.x / max(dist, 1.0);
    return clamp((t - pc.dof.y) / (1.0 - pc.dof.y), 0.0, 1.0) * pc.dof.z;
}
