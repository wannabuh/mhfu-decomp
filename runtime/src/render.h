/* The Vulkan renderer (render.c): the GE's primitives, as the software GE (gpu.c) prepares them - transformed, lit,
 * clipped, culled - drawn on the GPU, into render targets standing for the framebuffers in VRAM. */
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

/* As gpu.c's Vertex: screen position (PSP pixels), depth (0-65535), 1/w, texture coordinates (texels), colour (0-255),
 * fog (1 none, 0 all fog), position in the world, the sun's part of the colour (0-255) - or, lit by the pixel, the
 * material's diffuse colour - the normal in the world, and its motion in the world since the frame shown before (the
 * motion blur's: the hunter, monsters and people) */
typedef struct { float x, y, z, rw, u, v, r, g, b, a, fog, wx, wy, wz, sr, sg, sb, nx, ny, nz, mx, my, mz; } RVertex;

extern int render_on;                       /* the Vulkan renderer draws (else gpu.c does, in software) */
int render_init(void *sdl_window);          /* NULL: offscreen. 0 if Vulkan cannot be used */

/* gpu.c: before each primitive (the GE's state in regs), then its triangles and sprites */
void render_prim_begin(const uint32_t *regs);
void render_triangle(const RVertex *a, const RVertex *b, const RVertex *c);
void render_shadow_triangle(const RVertex *a, const RVertex *b, const RVertex *c);     /* clipped: for shadows */
void render_sprite(const RVertex *a, const RVertex *b);
void render_vram_written(uint32_t addr, uint32_t size);     /* something other than the GE wrote VRAM */

/* gpu.c, the GE's vertex stage on the GPU (MHFU_GPU_TL): whether it can be; a primitive (after render_prim_begin) of
 * type 3-5 (triangles, strip, fan), its draw (NULL: as the one before's), its nv vertices as the game gave them and its
 * count vertices' indices among them */
struct TlDrawS;                             /* (shared.h's TlDraw, TlVertex) */
struct TlVertexS;
int render_tl_ready(void);
int render_tl_tessellates(void);
/* (the draws lit in the world: where their lowest vertices are, nfeet of them - render_scene.c's feet - if wanted) */
int render_tl_feet(void);
void render_tl_draw(int type, int count, const struct TlDrawS *d, const struct TlVertexS *v, int nv, const uint16_t *idx,
                    const float *feet, int nfeet);
/* and for recorded frames (MHFU_FPS): a frame's vertices and triangles' indices stored in a slot (one of 4) as it is
 * recorded - indices among the slot's vertices, from vfirst and ifirst -, and drawn from it as it is replayed: n
 * indices from first, with a draw (NULL: the last's) */
void render_tl_store(int slot, uint32_t vfirst, const struct TlVertexS *v, int nv, uint32_t ifirst, const uint32_t *idx,
                     int ni);
void render_tl_slot_draw(int slot, const struct TlDrawS *d, uint32_t first, uint32_t n, const float *feet, int nfeet);

/* present.c: show the framebuffer at addr; read it back (480 x 272, ABGR8888) for frame dumps */
void render_flip(uint32_t addr);
void render_window_resized(void);           /* window.c: its size changed (the picture follows it) */
void render_finish_frame(void);             /* a frame not shown (fast-forwarding) */
void render_readback(uint32_t addr, uint32_t *pixels);

/* gpu.c, for textures: decode the current texture (level 0) into w x h ABGR8888 */
void gpu_decode_texture(uint32_t *out, int w, int h, const uint32_t *reg);   /* as reg has it */

/* Frames recorded for interpolation (gpu.c) */
void gpu_record(int on);
void gpu_frame_end(uint32_t display_addr, uint64_t time);
uint32_t gpu_render_frame(float t);
void gpu_match_stats(unsigned long *matched, unsigned long *unmatched);
uint64_t gpu_frame_period(void);
uint64_t gpu_last_frame_time(void);

/* The texture for the GE's state (made or updated now): its slot and size; and a slot to use instead of finding one,
 * for replaying (0xFFFFFFFE: none) */
uint32_t render_texture(const uint32_t *regs, int *w, int *h);
void render_override_texture(uint32_t slot, int w, int h);

#endif
