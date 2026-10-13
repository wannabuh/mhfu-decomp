/* The Vulkan renderer (render.c): the GE's primitives, as the software GE (gpu.c) prepares them - transformed, lit,
 * clipped, culled - drawn on the GPU, into render targets standing for the framebuffers in VRAM. */
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

/* As gpu.c's Vertex: screen position (PSP pixels), depth (0-65535), 1/w, texture coordinates (texels), colour (0-255),
 * fog (1 none, 0 all fog) */
typedef struct { float x, y, z, rw, u, v, r, g, b, a, fog; } RVertex;

extern int render_on;                       /* the Vulkan renderer draws (else gpu.c does, in software) */
int render_init(void *sdl_window);          /* NULL: offscreen. 0 if Vulkan cannot be used */

/* gpu.c: before each primitive (the GE's state in regs), then its triangles and sprites */
void render_prim_begin(const uint32_t *regs);
void render_triangle(const RVertex *a, const RVertex *b, const RVertex *c);
void render_sprite(const RVertex *a, const RVertex *b);
void render_vram_written(uint32_t addr, uint32_t size);     /* something other than the GE wrote VRAM */

/* present.c: show the framebuffer at addr; read it back (480 x 272, ABGR8888) for frame dumps */
void render_flip(uint32_t addr);
void render_readback(uint32_t addr, uint32_t *pixels);

/* gpu.c, for textures: decode the current texture (level 0) into w x h ABGR8888 */
void gpu_decode_texture(uint32_t *out, int w, int h);

#endif
