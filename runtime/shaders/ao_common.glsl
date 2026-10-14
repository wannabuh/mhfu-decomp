// Ambient occlusion (render.c): what its passes share - the world's depth and how to place a pixel of it in the
// camera's space.
layout(set = 0, binding = 0) uniform sampler2D source;  // the pass's input (the depth, or the occlusion)
layout(set = 0, binding = 1) uniform sampler2D depth;   // the render target's depth (full resolution)

layout(push_constant) uniform Ao {
    mat4 to_view;       // (PSP pixel x, y, depth, 1) to the camera's space (world units)
    vec4 params;        // radius (world units), strength, render target pixels a PSP pixel, pixels across one world unit
                        // at a distance of one (full resolution)
    vec4 params2;       // the distance it fades out by, blur direction (x, y), unused
    vec4 area;          // the picture's size in render target pixels (full resolution: x, y; half: z, w)
    vec4 lights[4];     // the point lights in the camera's space, and their reach (0: none)
    vec4 colors[4];
} pc;

// A pixel of the render target (full resolution), in the camera's space
vec3 view_pos(ivec2 pix) {
    float d = texelFetch(depth, pix, 0).r;
    vec4 v = pc.to_view * vec4((vec2(pix) + 0.5) / pc.params.z, d, 1.0);
    return v.xyz / v.w;
}
