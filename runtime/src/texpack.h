/* Texture packs (texpack.c): PPSSPP's, images drawn instead of the game's textures */
#ifndef TEXPACK_H
#define TEXPACK_H

#include <stdint.h>

enum { TEXPACK_FILTER_GAME, TEXPACK_FILTER_NEAREST, TEXPACK_FILTER_LINEAR };

typedef struct {
    uint32_t *pixels;               /* RGBA (as the game's decoded textures), the caller's to free */
    int w, h;
    int filter;
} TexpackImage;

void texpack_init(void);
int texpack_active(void);                                   /* a pack, or dumping */
void texpack_clut_loaded(uint32_t addr, uint32_t bytes);    /* gpu.c: each palette load, as PPSSPP keeps it */
/* The current texture's key as PPSSPP has it (max_seen_v: in 2D, the furthest down a 512-tall texture is used) */
int texpack_key(const uint32_t *reg, int max_seen_v, uint64_t *cachekey, uint32_t *hash);
/* Its replacement: 1 with it in out, 0 being read (ask again), -1 none */
int texpack_find(uint64_t cachekey, uint32_t hash, int w, int h, TexpackImage *out);
extern int texpack_draw_v;          /* gpu.c: how far down (texels) the draw uses a 512-tall texture; 512 unknown */
void texpack_dump(uint64_t cachekey, uint32_t hash, const uint32_t *px, int w, int h);

#endif
