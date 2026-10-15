/* What the renderer (render*.c, post.c) and its shaders share, written once: the binding numbers of the GE's
 * descriptor set, the draw flags, and the layouts of the push constants and light sets. Included by both C and GLSL
 * (glslang defines VULKAN): the members are written with the macros below, which make C's types or GLSL's.
 *
 * A layout holds only scalars, vec2 (first), vec4 and mat4, in an order that leaves no gap in either language. */
#ifndef SHARED_H
#define SHARED_H

#ifdef VULKAN
#define U32 uint
#define I32 int
#define F32 float
#define VEC2(n) vec2 n
#define VEC4(n) vec4 n
#define VEC4S(n, k) vec4 n[k]
#define MAT4(n) mat4 n
#else
#include <stdint.h>
#define U32 uint32_t
#define I32 int32_t
#define F32 float
#define VEC2(n) float n[2]
#define VEC4(n) float n[4]
#define VEC4S(n, k) float n[k][4]
#define MAT4(n) float n[16]
#endif

/* -- the GE's descriptor set (render.c) -------------------------------------------------------------------------- */
#define BIND_TEXTURES 0         /* sampler2D[]: the game's textures and the render targets, bindless */
#define BIND_SHADOW_MAP 1       /* sampler2DShadow: the sun's */
#define BIND_LIGHTS 2           /* LightSet[]: the draws' lights (and cameras, the point lights) */
#define BIND_POINT_SHADOWS 3    /* samplerCube[2]: the two strongest point lights' */
#define BIND_TRAIL 4            /* uint[]: the trodden grass (grass.c grass_trail) */
#define BIND_AO 5               /* sampler2D: the world's ambient occlusion, for the grass */

/* -- a draw's flags (DrawPush.flags) ------------------------------------------------------------------------------ */
#define DRAW_TFUNC 7u           /* the texture function (the GE's: modulate, decal, blend, replace, add) */
#define DRAW_TEX_ALPHA 8u       /* the texture's alpha is used */
#define DRAW_DOUBLE 16u         /* colour doubling */
#define DRAW_ALPHA_TEST 32u
#define DRAW_CLEAR 64u          /* clear mode */
#define DRAW_FOG 128u
#define DRAW_WORLD 256u         /* in the world: shadows fall on it, it casts them */
#define DRAW_LIT_GE 512u        /* lit by the GE, by the vertex (its sun part known) */
#define DRAW_LIT_PIXEL 1024u    /* lit by the pixel (light_set): the hunter, monsters, people */
#define DRAW_LIT (DRAW_LIT_GE | DRAW_LIT_PIXEL)
#define DRAW_KIND (DRAW_WORLD | DRAW_LIT)   /* (& DRAW_KIND == DRAW_WORLD: the scenery, lit beforehand) */
#define DRAW_BLENDED 2048u      /* (the shadow passes) blended by its alpha: casts where more than half opaque */
#define DRAW_SHADED 4096u       /* the scenery, shaded by the sun with normals made from its triangles */
#define DRAW_RELIEF 8192u       /* and relief from its texture's brightness */
#define DRAW_MOTION 16384u      /* (the motion blur's pass) its motion, not its colour */

/* -- push constants ----------------------------------------------------------------------------------------------- */
/* The GE's draws (ge.*, shadow.*, pshadow.*): the texture's size (texels) and index (~0u: none), the flags, the alpha
 * test (function bits 0-2, reference 8-15, mask 16-23), the environment and fog colours (RGB), the light set (~0u:
 * none); the world to the shadow map (x, y in -1..1, depth 0..1 away from the sun); the point lights' sets (~0u: none),
 * the set holding the world's camera for the tessellated draws (~0u: none) */
#define DRAW_PUSH                                                                                                      \
    VEC2(tex_size);                                                                                                    \
    U32 tex_index, flags, alpha_test, env_color, fog_color, light_set;                                                 \
    MAT4(light);                                                                                                       \
    U32 points, camera, pad0, pad1;

/* The grass (grass.vert, grass.frag): the world to the shadow map; the light set holding the world's camera (see
 * grass.vert), this frame's trodden grass (its first cell in the trail buffer, its window's first cell x, z); whether
 * BIND_AO holds this picture's occlusion */
#define GRASS_PUSH                                                                                                     \
    MAT4(light);                                                                                                       \
    U32 camera, trail;                                                                                                 \
    I32 trail_x, trail_z;                                                                                              \
    U32 ao;

/* Ambient occlusion's passes (ao*.frag): (PSP pixel x, y, depth, 1) to the camera's space (world units); radius (world
 * units), strength, render target pixels a PSP pixel, pixels across one world unit at a distance of one; the distance
 * it fades out by, the blur's direction (x, y); the picture's size (full resolution x, y; half z, w); the point lights
 * in the camera's space and their reach (0: none), their colours */
#define AO_PUSH                                                                                                        \
    MAT4(to_view);                                                                                                     \
    VEC4(params);                                                                                                      \
    VEC4(params2);                                                                                                     \
    VEC4(area);                                                                                                        \
    VEC4S(lights, 4);                                                                                                  \
    VEC4S(colors, 4);

/* Post-processing's passes (post*.frag and the rest of post.c's): (PSP pixel x, y, depth, 1) to the camera's space
 * (world units); the camera's space to the shadow map; the source's texel size (x, y), the first bloom pass (z); the
 * picture's size in pixels (x, y), render target pixels a PSP pixel (z); the direction to the sun (camera space), the
 * shafts' strength (w; 0: none); its colour, the shafts' contrast (w); the bloom's strength, the threshold of the
 * picture's own light, how much of it, the glow's gain; saturation, contrast, warmth, vignette; the depth of field's
 * focus distance, the share of it beyond which the blur begins, the largest blur (half resolution pixels; 0: none),
 * the sharpening (w: 0 none .. 1) */
#define POST_PUSH                                                                                                      \
    MAT4(to_view);                                                                                                     \
    MAT4(to_shadow);                                                                                                   \
    VEC4(texel);                                                                                                       \
    VEC4(area);                                                                                                        \
    VEC4(sun);                                                                                                         \
    VEC4(sun_color);                                                                                                   \
    VEC4(bloom);                                                                                                       \
    VEC4(grade);                                                                                                       \
    VEC4(dof);

/* -- light sets (BIND_LIGHTS) ------------------------------------------------------------------------------------- */
/* A draw's directional lights, in the world: the direction to the light (w: a power of the diffuse term, 0 none) and
 * colour, how many, which is the sun (99: none). The same layout holds other things: the point lights (dir: place and
 * reach; pad0 of the first set: how many have shadows) and cameras (dir: the world to the clip space, by columns; for
 * the grass, color too - see grass.vert) */
#define LIGHT_SET                                                                                                      \
    VEC4S(dir, 4);                                                                                                     \
    VEC4S(color, 4);                                                                                                   \
    U32 count, sun, pad0, pad1;

#ifdef VULKAN
struct LightSet { LIGHT_SET };
#else
typedef struct { DRAW_PUSH } DrawPush;
typedef struct { GRASS_PUSH } GrassPush;
typedef struct { AO_PUSH } AoPush;
typedef struct { POST_PUSH } PostPush;
typedef struct { LIGHT_SET } LightSet;
_Static_assert(sizeof(DrawPush) == 112, "DrawPush: as GLSL lays it out");
_Static_assert(sizeof(GrassPush) == 84, "GrassPush: as GLSL lays it out");
_Static_assert(sizeof(AoPush) == 240, "AoPush: as GLSL lays it out");
_Static_assert(sizeof(PostPush) == 240, "PostPush: as GLSL lays it out");
_Static_assert(sizeof(LightSet) == 144, "LightSet: as GLSL lays it out");
#endif

#endif
