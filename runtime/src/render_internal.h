/* The renderer's parts (render*.c) and what they share. render.h is what the rest of the runtime sees.
 *
 *   render.c          the core: setup, render targets, the GE's state as draws, batching vertices, flushing a batch,
 *                     frames and presenting
 *   render_texture.c  the game's textures, decoded and cached; what is learned from them (average colour, holes)
 *   render_scene.c    what is known of the world being drawn (struct Scene): its camera, the sun, the fog, the feet
 *                     of those walking in it, the moment shown - for the effects to use
 *   render_lights.c   light sets (the GE's lights for lighting by the pixel), and the point lights fire makes
 *   render_shadow.c   the sun's shadow map and the point lights' cube maps, drawn from the world's draws
 *   render_shading.c  the scenery's normals (sun shading), the characters' shape normals (tessellation), the wind
 *   render_ao.c       ambient occlusion
 *   render_grass.c    the ground grass's drawing (grass.c makes the blades)
 *   render_post.c     the glow and motion draws, and post.c's passes at the end of the world
 *
 * Shared with the shaders (binding numbers, draw flags, push constants): shared.h. */
#ifndef RENDER_INTERNAL_H
#define RENDER_INTERNAL_H

#include "render.h"
#include "shared.h"
#include "vk.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define RT_SIZE 512                 /* PSP pixels, square */
#define MAX_TARGETS 32
#define MAX_TEXTURES 4096
#define VERTEX_BYTES (64u << 20)
#define STAGING_BYTES (64u << 20)
#define LIGHT_SETS 8192             /* light sets a frame (the last two: the point lights) */
#define MAX_FEET 64

/* A vertex as the GPU takes it (ge.vert): clip position, texture coordinates (texels), colour, fog, place in the world,
 * the sun's part of the colour (or the material's colour, lit by the pixel; its fourth byte: tessellation's straight
 * edges), normal (snorm16; its fourth: the shape normal, tessellated), motion (half floats) since the frame shown
 * before */
typedef struct GpuVertex {
    float x, y, z, w, u, v;
    uint32_t color;
    float fog, wx, wy, wz;
    uint32_t sun;
    int16_t n[4];
    uint16_t motion[4];
} GpuVertex;

/* A render target: one per framebuffer address in VRAM, RT_SIZE PSP pixels square at the internal resolution */
typedef struct Target {
    uint32_t addr;                  /* VRAM address, mirror bits off */
    VkImg color, depth;
    int used, cleared;
    uint32_t sample_slot;           /* its slot in the texture array, when sampled */
} Target;

/* A draw's state: everything that, changed, needs a new draw */
typedef struct {
    Target *target;
    uint32_t tex_slot;
    float tex_w, tex_h;
    uint32_t flags, alpha_test, env_color, fog_color;    /* (flags: shared.h's DRAW_*) */
    VkBool32 blend;
    VkColorBlendEquationEXT eq;
    float blend_const[4];
    VkBool32 depth_test, depth_write;
    VkCompareOp depth_op;
    VkColorComponentFlags write_mask;
    VkRect2D scissor;
    VkCullModeFlags cull;
    VkFrontFace front;
    uint32_t lights;                /* its light set (by the pixel), ~0: none */
} DrawState;

/* -- render.c ----------------------------------------------------------------------------------------------------- */
extern float render_scale;          /* render target pixels a PSP pixel (MHFU_SCALE, or MHFU_RESOLUTION's lines) */
extern float render_sun_strength;
extern float render_wide;           /* and so many times that across: 1, or a little more where the picture fills a
                                     * screen a little wider than the PSP's (16:9: 1.0074) */
extern VkPipelineLayout ge_layout;  /* the GE's draws' (and the shadow passes', the grass's): ge_set, DrawPush */
extern VkDescriptorSet ge_set;
extern VkShaderStageFlags pc_stages;    /* the stages the push constants are for */
extern VkBuf vertices, staging;     /* the frame's (one of two: vk.frame) */
extern VkDeviceSize staging_used, batch_start;  /* the staging buffer's used; the batch not drawn yet's first */
extern VkSampler samplers[8];       /* nearest or linear (bit 0), clamped across (1), down (2); else repeating */
extern float render_jitter[2];     /* the world drawn this far off this frame (pixels; the anti-aliasing's) */
extern unsigned long frame_no;      /* the frames made, counted from 1 */
extern float prim_view[16], prim_world_view[12];   /* the primitive's camera: its space to the screen, to the world */

/* PSP pixels in render target pixels */
static inline uint32_t scaled(int psp) {
    return (uint32_t)lroundf((float)psp * render_scale);
}
static inline uint32_t scaled_x(int psp) {      /* (across) */
    return (uint32_t)lroundf((float)psp * render_scale * render_wide);
}
VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout pl, VkFormat color, VkFormat depth,
                         int kind);
void flush(void);                   /* draw what has been batched */
void begin_rendering(Target *t);
void end_rendering(void);
void end_rendering_kept(void);         /* ended with what is batched left for later */
Target *rendering_target(void);     /* the target being rendered into, or NULL */
Target *target_at(uint32_t addr, int create);  /* the render target at a VRAM address (made if create) */
void ge_rebind(void);               /* after drawing something else in the GE's pass: its pipeline and vertices again */
void ge_lost(void);                 /* after a pass of something else: the GE's state to set again */
void retire(VkImg *i);              /* an image the GPU may still be drawing with: freed when the frame comes round */
void write_slot(uint32_t slot, VkImageView view, VkSampler sampler);
void unpack_rgb(uint32_t c, float *f);
void mul4(const float *a, const float *b, float *out);     /* out = a b (4 x 4, by columns) */
int invert4(const float *m, float *out);
/* the world to the screen (PSP pixels, depth over 65535), and to the clip space as put places vertices, with a camera:
 * its space to the screen, to the world (3 x 4); 0 if it cannot be inverted */
int world_to_screen(const float *view, const float *world_view, float *m);
int world_to_clip(const float *view, const float *world_view, float *clip);

/* The GE's vertex stage on the GPU (MHFU_GPU_TL): the vertices (TlVertex, at binding 1) and triangles' indices; a
 * batch's segments - count indices from first, drawn with record rec (the instance) */
extern VkBuf tl_vertices, tl_indices;
typedef struct { uint32_t first, count, rec; } TlSegment;

/* -- render_texture.c --------------------------------------------------------------------------------------------- */
#define HOLE_GRID 16
extern float tex_average[MAX_TEXTURES][3];  /* a texture's average colour (by alpha) */
extern uint8_t tex_cutout[MAX_TEXTURES];    /* it has holes (the alpha test's: leaves, grass) */
extern uint8_t (*tex_holes)[HOLE_GRID * HOLE_GRID];    /* where: each of 16 x 16 parts' share of holes (0-255) - if it
                                                         * is a plant's colour (green, olive, dry brown), else 0 */
extern uint32_t *tex_small[MAX_TEXTURES];   /* small copies (at most 64 x 64), for the grass (MHFU_GRASS) */
extern uint8_t tex_small_w[MAX_TEXTURES], tex_small_h[MAX_TEXTURES];
void texture_init(void);
uint32_t texture_new_slot(void);
uint32_t texture_wound_slot(void);  /* the picture deep wounds are drawn from (render_texture.c) */    /* a slot in the texture array */
uint32_t texture_slot(const uint32_t *reg, int *w, int *h);    /* the GE's current texture's (made or updated) */

/* -- render_scene.c ----------------------------------------------------------------------------------------------- */
/* A foot of what the GE lights (the hunter, monsters, people): the middle and spread of a lit batch's lowest corners,
 * its velocity (units a second) */
typedef struct { float x, y, z, r, vx, vz; } Foot;
typedef struct {
    /* the camera the solid world being drawn was drawn with (the last solid draw's, until its occlusion is applied):
     * its space to the screen (PSP pixels, depth over 65535) and to the world (3 x 4, by columns: its last column the
     * eye) */
    float view[16], world_view[12];
    /* the sun: the world to the shadow map (by columns), its colour (the GE's light that is the sun), the point the
     * camera looks at (the shadow map's centre) */
    float sun_vp[16], sun_rgb[3], focus[3];
    /* the solid world's fog (end, slope, on), its colour, its depth test (the last solid draw's) */
    float fog[3];
    uint32_t fog_color;
    VkCompareOp depth_op;
    /* the feet found in the last frame drawn (whole) */
    Foot feet[MAX_FEET];
    int nfeet;
} Scene;
extern Scene scene;
double scene_time(void);            /* the moment shown, in seconds of the game's */
void scene_lit_vertex(size_t i, const float *w);   /* a lit batch's vertex i, in the world (its feet are found) */
void scene_lit_batch(size_t n);     /* the lit batch of n vertices, flushed */
void scene_end_frame(void);

/* -- render_lights.c ---------------------------------------------------------------------------------------------- */
extern VkBuf light_buf;             /* the light sets: LIGHT_SETS a frame in flight */
extern uint32_t light_sets_used;
extern int point_lights_on;         /* MHFU_POINT_LIGHTS (0: none) */
extern int emitting;                /* the primitive being batched adds light (fire): its light is gathered */
void lights_init(void);
void lights_settings(void);
uint32_t prim_light_set(void);      /* the primitive's lights' set, ~0: none */
uint32_t camera_light_set(void);    /* a set holding the world to the clip space (the primitive's camera); ~0: none */
uint32_t point_light_set(void);     /* the point lights' (two sets from it) */
void lights_emit(const float *w, uint32_t r, uint32_t g, uint32_t b, uint32_t a);    /* a vertex of what emits */
void lights_flush_emitter(uint32_t tex_slot);      /* the primitive's light, gathered (tex_slot: its texture's) */
const LightSet *lights_points(void);    /* this frame's point lights (two sets) */
int lights_end_frame(uint32_t tex_slot);   /* how many point lights have shadows next frame */

/* -- render_shadow.c ---------------------------------------------------------------------------------------------- */
extern int shadow_on, shadow_drawn; /* the sun's shadows are on this frame; the map was drawn this frame */
extern int point_shadows_on;        /* MHFU_POINT_SHADOWS */
extern VkImg shadow_img;
extern VkSampler shadow_sampler, point_sampler;
void shadow_init(void);
void shadow_settings(void);
void shadow_draw(const DrawState *s, DrawPush pc, uint32_t first, uint32_t n);  /* a batch, into the shadow maps */
void shadow_draw_tl(const DrawState *s, DrawPush pc, const TlSegment *seg, int nseg);  /* (the GPU's vertex stage's) */
void shadow_skip(void);             /* a batch not drawn */
void shadow_extra_triangle(const RVertex *a, const RVertex *b, const RVertex *c);
void shadow_frame_reused(int frame);
void shadow_end_frame(int next_point_shadows);

/* -- render_shading.c --------------------------------------------------------------------------------------------- */
extern float sway;                  /* the wind's strength (MHFU_SWAY; 0 none) */
extern int sun_shading, relief_on;  /* MHFU_SUN_SHADING, MHFU_RELIEF */
void shading_settings(void);
void shading_keep(size_t i, const GpuVertex *g);   /* the batch's vertex i as put (swaying, tessellated) */
void shading_corner(size_t i, const float *w, float sx, float sy);  /* the scenery's: world, screen (PSP pixels) */
const GpuVertex *shading_kept(void);    /* the batch's vertices as put (kept: swaying, tessellated) */
void make_normals(size_t n);
void shape_normals(size_t n);
float on_screen_size(size_t n);
void sway_batch(const DrawState *d, size_t n);

/* -- render_ao.c -------------------------------------------------------------------------------------------------- */
void ao_init(void);
void ao_rescale(void);              /* the internal resolution changed (the GPU idle): its images again */
void ao_state(Target *t, int solid, int clear);    /* a primitive drawn into t (solid: of the solid world) */
void ao_finish(void);               /* the frame shown or read: any occlusion still to apply */
int ao_ready(void);                 /* its image holds the occlusion of the picture being drawn (for the grass) */
VkImageView ao_image(void);
void ao_end_frame(void);
void ao_water_setup(VkShaderModule vs, VkShaderModule tl_vs);   /* the pipelines marking the water */
void ao_keep(Target *t, const DrawState *s, const DrawPush *pc, uint32_t first, uint32_t n, const TlSegment *seg);

/* -- render_grass.c ----------------------------------------------------------------------------------------------- */
extern int grass_on, grass_made;
void grass_setting(void);
void grass_init(void);
void grass_bind_ao(void);           /* render_ao.c's image (re-made) bound for the blades */
int grass_laying(void);             /* this frame lays the grass's ground */
void grass_scenery(const DrawState *d, const RVertex *a, const RVertex *b, const RVertex *c);
void grass_draw(Target *t);

/* -- render_water.c ----------------------------------------------------------------------------------------------- */
extern int water_on;                /* MHFU_WATER */
void water_init(void);
void water_settings(void);
int water_prim(const uint32_t *reg, const DrawState *s);   /* the primitive is water (its picture copied if first) */
VkImageView water_under(Target *t);
uint32_t water_light_set(void);     /* the water's light sets (two), ~0: none */

/* -- render_post.c ------------------------------------------------------------------------------------------------ */
void post_setup(VkShaderModule vs, VkShaderModule fs, int tess, VkShaderModule tl_vs);
void post_state(Target *t, uint32_t flags, int solid, int clear, int same);  /* a primitive drawn into t (same: batched with the last) */
void post_keep(Target *t, const DrawState *s, const DrawPush *pc, uint32_t first, uint32_t n, int tessellated);
void post_keep_tl(Target *t, const DrawState *s, const DrawPush *pc, const TlSegment *seg, int nseg);
void post_finish(void);
void post_frame_done(void);

#endif
