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
#define BIND_SCENE_COLOR 6      /* sampler2D: the picture under the water (render_water.c), and its depth */
#define BIND_SCENE_DEPTH 7
#define BIND_TL 8               /* TlDraw[]: the draws whose vertex stage the GPU does (ge_tl.vert), half a frame */
#define BIND_BLOOD 10           /* uint[]: the ground's blood (blood.c) */
#define BIND_RUNS 11            /* uint[]: the blood run down the animals (trails.c) */
#define BIND_TL_EXTRA 9         /* vec4[]: their vertices' part in the scenery's effects (render.c tl_scenery) */

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
#define DRAW_WATER 32768u       /* water (render_water.c): DrawPush.camera its light sets */
#define DRAW_SWAY 65536u        /* (the GPU's vertex stage) moved by the wind: DrawPush.camera its TL_WIND */
#define DRAW_FACES 131072u      /* (the GPU's vertex stage) shaded: the normals summed where corners meet, each
                                 * triangle's own where they bend more than 60 degrees from it (ge.frag) */
#define DRAW_NO_CAST 262144u    /* far scenery (gpu.c backdrop_draw): it casts no shadows */

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
 * the sharpening (w: 0 none .. 1); the world's up in the camera's space, and (w, its bits) the haze's colour (RGB, 8 bits
 * each) and strength (the top byte, 64 to 1; 0: none) */
#define POST_PUSH                                                                                                      \
    MAT4(to_view);                                                                                                     \
    MAT4(to_shadow);                                                                                                   \
    VEC4(texel);                                                                                                       \
    VEC4(area);                                                                                                        \
    VEC4(sun);                                                                                                         \
    VEC4(sun_color);                                                                                                   \
    VEC4(bloom);                                                                                                       \
    VEC4(grade);                                                                                                       \
    VEC4(dof);                                                                                                         \
    VEC4(haze);

/* -- light sets (BIND_LIGHTS) ------------------------------------------------------------------------------------- */
/* A draw's directional lights, in the world: the direction to the light (w: a power of the diffuse term, 0 none) and
 * colour, how many, which is the sun (99: none). The same layout holds other things: the point lights (dir: place and
 * reach; pad0 of the first set: how many have shadows) and cameras (dir: the world to the clip space, by columns; for
 * the grass, color too - see grass.vert) */
/* Blood off the animal (blood.c, render_lights.c). The light sets from BLOOD_SET_AT: the drops in the air - where
 * each is (dir, w: how wide) and its colour, the first's count how many; as many sets with each one's streak (dir: back
 * to where it was); two with the ground's blood's 16 colours (dir, then color); one with the eye's place (dir[0];
 * dir[1]: the look of the blood on an animal - x how solid, 0 to 1, y how it shines, times, z the moment the
 * blood run down it is at (RUN_ below); its
 * count: whether there is any blood on the ground). BIND_BLOOD: the ground's blood, BLOOD_N x BLOOD_N cells of
 * BLOOD_UNIT units of the world, wrapped - a cell's low 10 bits how much (255 covers the ground), then 8 how wet, 10
 * its ground's height (8 units a step, modulo; BLOOD_ANY: any ground under the eye's reach), 4 its colour */
#define BLOOD_DROP_SETS 6
#define BLOOD_SETS (2 * BLOOD_DROP_SETS + 3)
#define BLOOD_SET_AT 8175u
#define BLOOD_N 4096
#define BLOOD_UNIT 2.5f
#define BLOOD_ANY 1023

/* Blood run down an animal (trails.c, BIND_RUNS): for each of RUN_MESHES skins RUN_TRIS words, a triangle's the first
 * of its pieces (1 the first of the RUN_ENTRIES; 0 none) - a draw's record says where its triangles' words begin, less
 * one (TlDraw runs; 0: it has none). Then the pieces, RUN_WORDS words each: where it begins among its triangle's
 * vertices as the game gave them (three floats) and half how wide it is there, in their units; the same where it ends;
 * how far along its trickle it begins and ends (units of the world), and when the blood came to each end (seconds,
 * as the eye's set's dir[1].z has now); the triangle's next piece, its colour (RGB8; bit 24: a bead, the trickle's
 * end), a number of its own, and how many of the vertices' units one of the world's is here */
#define RUN_MESHES 4
#define RUN_TRIS 16384
#define RUN_ENTRIES 8192
#define RUN_WORDS 16
/* Then the blood kept texel by texel (bloodmap.c): from RUN_MAPS_AT each skin's blood's colour (RUN_MESHES words:
 * RGB8; bit 24: its map shown in squares, to look at), then for each triangle of each skin RUN_MAP_WORDS words - two
 * rows of four floats, a place among its vertices as the game gave them to its texel in the maps (x, y; all zero: it
 * has none) - and from RUN_TEXELS_AT the maps, one under another, BLOODMAP_W texels wide and BLOODMAP_H high each: a
 * texel's low 8 bits how much blood, the next 8 how thick it lies, then 8 the moment it last got wet (seconds, 1 to
 * 255 going round, as the eye's set's dir[1].w has now; 0: dry long since), then 4 how open a cut is there (its lips
 * low, its inside high) and 4 how bruised the skin */
#define RUN_MAP_WORDS 8
#define BLOODMAP_W 2048
#define BLOODMAP_H 2048
#define RUN_MAPS_AT (RUN_MESHES * RUN_TRIS + RUN_ENTRIES * RUN_WORDS)
#define RUN_TEXELS_AT (RUN_MAPS_AT + RUN_MESHES + RUN_MESHES * RUN_TRIS * RUN_MAP_WORDS)
#define RUN_ALL (RUN_TEXELS_AT + RUN_MESHES * BLOODMAP_W * BLOODMAP_H)

#define LIGHT_SET                                                                                                      \
    VEC4S(dir, 4);                                                                                                     \
    VEC4S(color, 4);                                                                                                   \
    U32 count, sun, pad0, pad1;

/* -- the GE's vertex stage on the GPU (BIND_TL, ge_tl.vert) -------------------------------------------------------- */
/* A draw's vertex stage, as gpu.c's vertex() and light() have it: the world, view and projection matrices (3 x 4 by
 * rows: x, y, z each a row's dot with (p, 1); the projection 4 x 4 by columns), the GE's space to the world (and for
 * the motion blur, the world matrix a frame shown earlier); the viewport's scale (x, y, z) and depth offset (w), its
 * x, y offsets (the screen offset in), the fog's end and slope; the texture coordinates' scale and offset (in texels);
 * the material's emissive colour (w: the specular power), ambient (w: its alpha), diffuse and specular colours, the
 * global ambient light (w: its alpha); the flags (TL_*), the material mode (0x53), the light that is the sun (~0u:
 * none), the bones; each light's position or direction (w: its type), its spot direction (w: its components), its
 * attenuation (w: the spot's cutoff), ambient (w: the spot's exponent), diffuse and specular colours; the bones (3 x 4
 * by rows), and a frame shown earlier; its wounds (gpu.c: how many, at most 4, and for each where it is among the
 * vertices as the game gave them, the rows' w, and what takes a vertex's place from there to its place from the wound
 * in the wound's measure, 1 at its edge: three rows; and how it looks: its blood's colour as r + 256 g + 65536 b, how
 * much of it is left, how wet it is, its kind (0 a cut, 1 a shot's hole) plus a fraction of its own; and where its
 * blood runs: down across the skin as the cut's along and across see it, the cut's half length and half width) */
#define TL_DRAW                                                                                                        \
    VEC4S(world, 3);                                                                                                   \
    VEC4S(view, 3);                                                                                                    \
    MAT4(proj);                                                                                                        \
    VEC4S(to_world, 3);                                                                                                \
    VEC4S(earlier_world, 3);                                                                                           \
    VEC4(viewport);                                                                                                    \
    VEC4(offset);                                                                                                      \
    VEC4(tex);                                                                                                         \
    VEC4(emissive);                                                                                                    \
    VEC4(ambient);                                                                                                     \
    VEC4(diffuse);                                                                                                     \
    VEC4(specular);                                                                                                    \
    VEC4(global);                                                                                                      \
    U32 flags, mode, sun_light, bones;                                                                                 \
    VEC4S(light_pos, 4);                                                                                               \
    VEC4S(light_dir, 4);                                                                                               \
    VEC4S(light_att, 4);                                                                                               \
    VEC4S(light_ambient, 4);                                                                                           \
    VEC4S(light_diffuse, 4);                                                                                           \
    VEC4S(light_specular, 4);                                                                                          \
    VEC4S(bone, 24);                                                                                                   \
    VEC4S(earlier_bone, 24);                                                                                           \
    U32 wounds, wound_pad0, runs, wound_pad2;                                                                          \
    VEC4S(wound, 12);                                                                                                  \
    VEC4S(wound_look, 4);                                                                                              \
    VEC4S(wound_run, 4);
#define TL_LIT 1u               /* lit by the GE */
#define TL_FLIP 2u              /* its normals turned round (0x51; only those the vertices have) */
#define TL_WORLD 4u             /* placed in the world (to_world) */
#define TL_EARLIER 8u           /* its motion since a frame shown earlier (earlier_*) */
#define TL_VCOL 16u             /* the vertices have colours */
#define TL_TEX 32u              /* texture coordinates */
#define TL_LIGHT_ON 256u        /* (4 bits) the lights on */
#define TL_LIGHT_PIXEL 4096u    /* (4 bits) the lights whose diffuse part the pixel does */
/* BIND_TL_EXTRA: each frame in flight's wind, TL_WIND vec4s - (its time, how far, how many feet), then the feet bending
 * the plants away (x, y, z, reach) - then a vec4 for each of the vertices (indexed as the vertex buffer): the sun
 * shading's normal in the world (DRAW_SHADED), or (x) the height above its feet the wind moves it by (DRAW_SWAY; 0:
 * held) */
#define TL_WIND 65u

#ifdef VULKAN
struct LightSet { LIGHT_SET };
struct TlDraw { TL_DRAW };
#else
typedef struct { DRAW_PUSH } DrawPush;
typedef struct { GRASS_PUSH } GrassPush;
typedef struct { AO_PUSH } AoPush;
typedef struct { POST_PUSH } PostPush;
typedef struct { LIGHT_SET } LightSet;
typedef struct TlDrawS { TL_DRAW } TlDraw;
/* A vertex as the GPU's vertex stage takes it (ge_tl.vert): as the game gave it, decoded - position, normal ((0, 0, 1)
 * if it has none), colour (RGBA8; the material's ambient if it has none), texture coordinates (before scaling), the
 * bones' weights; for tessellation (ge.tesc, ge.tese), the shape normal (the normals of the corners at its place,
 * averaged; 0 none) and - its triangle's third corner - which of its triangle's edges stay straight (bit k: the edge
 * facing corner k) */
typedef struct TlVertexS {
    float pos[3], normal[3];
    uint32_t color;
    float uv[2], weights[8], shape[3];
    uint32_t edges;
} TlVertex;
_Static_assert(sizeof(TlDraw) == 1888, "TlDraw: as GLSL lays it out");
_Static_assert(sizeof(TlVertex) == 84, "TlVertex: ge_tl.vert's attributes");
_Static_assert(sizeof(DrawPush) == 112, "DrawPush: as GLSL lays it out");
_Static_assert(sizeof(GrassPush) == 84, "GrassPush: as GLSL lays it out");
_Static_assert(sizeof(AoPush) == 240, "AoPush: as GLSL lays it out");
_Static_assert(sizeof(PostPush) == 256, "PostPush: as GLSL lays it out");
_Static_assert(sizeof(LightSet) == 144, "LightSet: as GLSL lays it out");
#endif

#endif
