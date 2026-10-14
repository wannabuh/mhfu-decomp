/* The Vulkan renderer (render.c): the GE's primitives, as the software GE (gpu.c) prepares them - transformed, lit,
 * clipped, culled - drawn on the GPU, into render targets standing for the framebuffers in VRAM. */
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

/* As gpu.c's Vertex: screen position (PSP pixels), depth (0-65535), 1/w, texture coordinates (texels), colour (0-255),
 * fog (1 none, 0 all fog), position in the world, the sun's part of the colour (0-255) */
typedef struct { float x, y, z, rw, u, v, r, g, b, a, fog, wx, wy, wz, sr, sg, sb; } RVertex;

extern int render_on;                       /* the Vulkan renderer draws (else gpu.c does, in software) */
int render_init(void *sdl_window);          /* NULL: offscreen. 0 if Vulkan cannot be used */

/* gpu.c: before each primitive (the GE's state in regs), then its triangles and sprites */
void render_prim_begin(const uint32_t *regs);
void render_triangle(const RVertex *a, const RVertex *b, const RVertex *c);
void render_sprite(const RVertex *a, const RVertex *b);
void render_vram_written(uint32_t addr, uint32_t size);     /* something other than the GE wrote VRAM */

/* present.c: show the framebuffer at addr; read it back (480 x 272, ABGR8888) for frame dumps */
void render_flip(uint32_t addr);
void render_finish_frame(void);             /* a frame not shown (fast-forwarding) */
void render_readback(uint32_t addr, uint32_t *pixels);

/* gpu.c, for textures: decode the current texture (level 0) into w x h ABGR8888 */
void gpu_decode_texture(uint32_t *out, int w, int h);

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
