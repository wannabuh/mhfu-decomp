/* The game's textures (render_internal.h): decoded on the CPU (by gpu.c) and cached by address, format, size and a hash
 * of their data (checked once a frame), with mipmaps; a texture whose address is a render target's samples that render
 * target. What the effects learn from each as it is made: its average colour (the lights fire makes), whether it has
 * holes and where they are in a plant's colour (the wind), a small copy (the grass's ground). */
#include "render_internal.h"

#include "hle.h"
#include "texpack.h"

#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t key, hash;
    uint32_t addr;
    int w, h;
    VkImg img;
    int levels;                     /* its mipmaps' */
    uint32_t slot;
    unsigned long checked;          /* the frame it was last checked in */
    unsigned long used;
    int sampler;
    int pending;                    /* its texture pack image is being read */
    uint64_t pack_key;              /* (its key in the pack) */
    uint32_t pack_hash;
} Texture;

static Texture *textures;
static int ntextures;
static uint32_t next_slot = 0;
static int mipmaps = 1;            /* the game's textures get mipmaps (MHFU_MIPMAPS=0: none) */
/* A texture being made, with its smaller levels after it: in memory of our own, then copied into the staging buffer
 * whole (that is mapped and write-combined: slow to read - making levels from it took 150 ms a 512 x 512 texture) */
static uint32_t *scratch;
static size_t scratch_bytes;

static uint32_t *scratch_for(size_t bytes) {   /* (what it holds kept) */
    if (bytes > scratch_bytes) {
        scratch_bytes = bytes * 2;
        scratch = realloc(scratch, scratch_bytes);
    }
    return scratch;
}
float tex_average[MAX_TEXTURES][3];
uint8_t tex_cutout[MAX_TEXTURES];
uint8_t (*tex_holes)[HOLE_GRID * HOLE_GRID];
uint32_t *tex_small[MAX_TEXTURES];
uint8_t tex_small_w[MAX_TEXTURES], tex_small_h[MAX_TEXTURES];

/* The cache, and the samplers: nearest or linear, each axis repeating or clamped; with mipmaps, made smaller from their
 * levels (trilinear), and anisotropic where linear */
void texture_init(void) {
    mipmaps = !getenv("MHFU_MIPMAPS") || atoi(getenv("MHFU_MIPMAPS")) != 0;
    for (int i = 0; i < 8; i++) {
        VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = (i & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.addressModeU = (i & 2) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = (i & 4) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = 0;
        if (mipmaps) {
            sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.maxLod = VK_LOD_CLAMP_NONE;
            if (vk.anisotropy && (i & 1)) {
                sci.anisotropyEnable = VK_TRUE;
                sci.maxAnisotropy = 16.0f;
            }
        }
        vkCreateSampler(vk.device, &sci, NULL, &samplers[i]);
    }
    textures = calloc(MAX_TEXTURES, sizeof *textures);
    tex_holes = calloc(MAX_TEXTURES, sizeof *tex_holes);
    texpack_init();
}

uint32_t texture_new_slot(void) {
    return next_slot++;
}

/* -- textures ----------------------------------------------------------------------------------------------------- */

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (size_t i = n & ~(size_t)7; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* A texture's next smaller level (half each way, at least 1): each texel the average of four, its colour weighted by
 * their alpha (holes lend it none). A texture with holes (alpha tested: grass, leaves) keeps as much of itself solid
 * as the full size has - its alpha scaled to that - so that it does not thin away in the distance */
static void make_level(const uint32_t *src, uint32_t *dst, int w, int h, int cutout, float want) {
    int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
    int sx = w > 1 ? 2 : 1, sy = h > 1 ? 2 : 1;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            uint32_t sum[4] = {0, 0, 0, 0}, plain[3] = {0, 0, 0}, n = 0;
            for (int j = 0; j < sy; j++)
                for (int i = 0; i < sx; i++) {
                    uint32_t c = src[(y * sy + j) * w + x * sx + i], a = c >> 24;
                    for (int k = 0; k < 3; k++) {
                        sum[k] += ((c >> (8 * k)) & 255) * a;
                        plain[k] += (c >> (8 * k)) & 255;
                    }
                    sum[3] += a;
                    n++;
                }
            uint32_t out = 0;
            for (int k = 0; k < 3; k++)
                out |= (sum[3] ? (sum[k] + sum[3] / 2) / sum[3] : (plain[k] + n / 2) / n) << (8 * k);
            dst[y * dw + x] = out | ((sum[3] + n / 2) / n) << 24;
        }
    if (!cutout)
        return;
    /* the scale of this level's alpha keeping the share solid (alpha at least half) the full size has: want */
    size_t n = (size_t)dw * (size_t)dh;
    float lo = 0.0f, hi = 4.0f;
    for (int it = 0; it < 10; it++) {
        float mid = 0.5f * (lo + hi);
        size_t c = 0;
        for (size_t i = 0; i < n; i++)
            c += (float)(dst[i] >> 24) * mid >= 127.5f;
        if ((float)c / (float)n < want)
            lo = mid;
        else
            hi = mid;
    }
    for (size_t i = 0; i < n; i++) {
        float a = (float)(dst[i] >> 24) * hi;
        dst[i] = (dst[i] & 0xFFFFFFu) | (uint32_t)(a > 255.0f ? 255.0f : a + 0.5f) << 24;
    }
}

/* Level 0 (w x h, in scratch) to the texture, with its smaller levels made after it */
static void upload(Texture *t, int w, int h) {
    int sampler = t->sampler;
    size_t size = (size_t)w * (size_t)h * 4;
    /* its smaller levels, after it */
    int levels = 1;
    if (mipmaps)
        while ((w >> levels) || (h >> levels))
            levels++;
    size_t level_at[16] = {0};
    size_t total = size;
    for (int l = 1; l < levels; l++) {
        int lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        level_at[l] = total;
        total += (size_t)lw * (size_t)lh * 4;
    }
    if (staging_used + total > STAGING_BYTES) {
        levels = 1;
        total = size;
    }
    uint8_t *px = (uint8_t *)scratch_for(total);    /* (level 0 is there already: scratch_for keeps it) */
    int cutout = t->slot < MAX_TEXTURES && tex_cutout[t->slot];
    float want = 0;
    if (cutout && levels > 1) {         /* the share of the full size solid (alpha at least half) */
        size_t solid = 0;
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
            solid += (((const uint32_t *)px)[i] >> 24) >= 128;
        want = (float)solid / (float)((size_t)w * (size_t)h);
    }
    for (int l = 1; l < levels; l++)
        make_level((uint32_t *)(px + level_at[l - 1]), (uint32_t *)(px + level_at[l]),
                   w >> (l - 1) ? w >> (l - 1) : 1, h >> (l - 1) ? h >> (l - 1) : 1, cutout, want);
    memcpy((uint8_t *)staging.map + staging_used, px, total);
    if (t->w != w || t->h != h || t->levels != levels) {
        retire(&t->img);
        t->img = vk_image_mips((uint32_t)w, (uint32_t)h, (uint32_t)levels, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        t->levels = levels;
        t->w = w;
        t->h = h;
    }
    Target *was = rendering_target();
    end_rendering();
    vk_transition(&t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copies[16];
    for (int l = 0; l < levels; l++)
        copies[l] = (VkBufferImageCopy){staging_used + level_at[l], 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1},
            {0, 0, 0}, {w >> l ? (uint32_t)(w >> l) : 1u, h >> l ? (uint32_t)(h >> l) : 1u, 1}};
    vkCmdCopyBufferToImage(vk_cmd(), staging.buffer, t->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)levels, copies);
    vk_transition(&t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    staging_used += (total + 255) & ~(size_t)255;
    write_slot(t->slot, t->img.view, samplers[sampler]);
    if (was)
        begin_rendering(was);
}

/* -- the wound's picture ---------------------------------------------------------------------------------------------
 *
 * A deep wound (gpu.c's wounds, ge_frag.glsl) is drawn from a picture: 256 x 64, the cut running along it. Its alpha
 * is how much of the skin the wound covers; its colour the wound's own where the monster bleeds red, and only how
 * light it is where it bleeds another colour. wound.png in $XDG_CONFIG_HOME/mhfu-recomp/ (~/.config/...) if there is
 * one, any size; else one made here - a torn gash, dark inside, raw at its lips, blood drawn thin about it.
 * MHFU_WOUND_TEMPLATE=file.png: the one made here written there, to paint over. */
#define WOUND_W 256
#define WOUND_H 64

static float wound_hash(int x, int y) {
    uint32_t h = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (float)(h & 0xFFFF) / 65535.0f;
}

/* (noise that wanders smoothly: x, y in cells) */
static float wound_noise(float x, float y) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - (float)xi, fy = y - (float)yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = wound_hash(xi, yi), b = wound_hash(xi + 1, yi), c = wound_hash(xi, yi + 1), d = wound_hash(xi + 1, yi + 1);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

static void wound_made(uint32_t *px) {
    for (int y = 0; y < WOUND_H; y++)
        for (int x = 0; x < WOUND_W; x++) {
            float u = ((float)x + 0.5f) / WOUND_W * 2 - 1, v = ((float)y + 0.5f) / WOUND_H * 2 - 1;
            float taper = powf(fmaxf(1 - u * u, 0), 0.55f);
            /* the cut's middle wanders, its lips are torn: wide and fine noise along it */
            float mid = (wound_noise(u * 3.0f + 7, 0) - 0.5f) * 0.5f + (wound_noise(u * 11.0f, 3) - 0.5f) * 0.16f;
            float half = taper * (0.20f + 0.16f * wound_noise(u * 5.0f, 9) + 0.06f * wound_noise(u * 23.0f, 5));
            float d = fabsf(v - mid * taper);
            float torn = half + 0.05f * (wound_noise(u * 40.0f, v * 9.0f) - 0.5f) * taper;
            float inside = 1 - fminf(fmaxf((d - torn * 0.55f) / (torn * 0.2f + 1e-4f), 0), 1);     /* the dark of it */
            float lips = 1 - fminf(fmaxf((d - torn) / (torn * 0.25f + 1e-4f), 0), 1);              /* to its edge */
            /* blood about it: runs out a little way, further in strands across the cut */
            float run = taper * (0.42f + 0.3f * wound_noise(u * 6.0f + 3, 1) + 0.25f * wound_noise(u * 30.0f, 2));
            float about = 1 - fminf(fmaxf((d - torn) / (fmaxf(run - torn, 0.02f)), 0), 1);
            about *= 0.55f + 0.45f * wound_noise(u * 50.0f, v * 4.0f);
            float fibre = 0.75f + 0.5f * wound_noise(u * 14.0f, v * 30.0f);                      /* flesh, across the cut */
            float r = 0.62f * fibre, g = 0.07f * fibre, b = 0.06f * fibre;                           /* the lips: raw */
            r = r + (0.16f - r) * inside, g = g + (0.015f - g) * inside, b = b + (0.02f - b) * inside;
            float a = lips > 0 ? 0.92f * lips + (1 - lips) * 0.5f * about : 0.5f * about * about;
            if (lips <= 0)
                r = 0.42f, g = 0.03f, b = 0.03f;                                                   /* the blood about it */
            a *= taper > 0.02f ? 1 : 0;
            uint32_t R = (uint32_t)(fminf(r, 1) * 255), G = (uint32_t)(fminf(g, 1) * 255), B = (uint32_t)(fminf(b, 1) * 255);
            px[y * WOUND_W + x] = R | G << 8 | B << 16 | (uint32_t)(fminf(fmaxf(a, 0), 1) * 255) << 24;
        }
}

/* The wound's picture's slot (made at first asking: by the renderer, in a frame) */
uint32_t texture_wound_slot(void) {
    static Texture t;
    static int made;
    if (made)
        return t.slot;
    made = 1;
    int w = WOUND_W, h = WOUND_H;
    uint32_t *px = NULL;
    char path[1024] = "";
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0])
        snprintf(path, sizeof path, "%s/mhfu-recomp/wound.png", xdg);
    else if (home)
        snprintf(path, sizeof path, "%s/.config/mhfu-recomp/wound.png", home);
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    if (path[0] && png_image_begin_read_from_file(&img, path)) {
        img.format = PNG_FORMAT_RGBA;
        if (img.width >= 8 && img.width <= 2048 && img.height >= 2 && img.height <= 2048) {
            px = malloc(PNG_IMAGE_SIZE(img));
            if (px && png_image_finish_read(&img, NULL, px, 0, NULL)) {
                w = (int)img.width;
                h = (int)img.height;
                fprintf(stderr, "wounds: %s, %d x %d\n", path, w, h);
            } else {
                free(px);
                px = NULL;
            }
        }
        png_image_free(&img);
    }
    if (!px) {
        px = malloc(sizeof *px * WOUND_W * WOUND_H);
        wound_made(px);
        const char *out = getenv("MHFU_WOUND_TEMPLATE");
        if (out && out[0]) {
            memset(&img, 0, sizeof img);
            img.version = PNG_IMAGE_VERSION;
            img.width = WOUND_W;
            img.height = WOUND_H;
            img.format = PNG_FORMAT_RGBA;
            if (png_image_write_to_file(&img, out, 0, px, 0, NULL))
                fprintf(stderr, "wounds: the picture made here written to %s\n", out);
        }
    }
    memcpy(scratch_for((size_t)w * (size_t)h * 4 * 2), px, (size_t)w * (size_t)h * 4);
    free(px);
    t.slot = next_slot++;
    t.sampler = 1 | 2 | 4;              /* (linear, held at its edges) */
    upload(&t, w, h);
    return t.slot;
}

/* A texture pack's image for the texture, if it has one and it is in: to the staging buffer in place of the game's
 * texture. 0 if not (pending: not read yet) */
static int pack_image(Texture *t, int w, int h, int *iw, int *ih) {
    TexpackImage im;
    int r = texpack_find(t->pack_key, t->pack_hash, w, h, &im);
    t->pending = r == 0;
    if (r <= 0)
        return 0;
    size_t size = (size_t)im.w * (size_t)im.h * 4;
    if (staging_used + size * 4 / 3 + 1024 > STAGING_BYTES) {
        free(im.pixels);
        t->pending = 1;             /* (no room this frame: read again) */
        return 0;
    }
    memcpy(scratch_for(size), im.pixels, size);
    free(im.pixels);
    if (im.filter == TEXPACK_FILTER_LINEAR)
        t->sampler |= 1;
    else if (im.filter == TEXPACK_FILTER_NEAREST)
        t->sampler &= ~1;
    *iw = im.w;
    *ih = im.h;
    return 1;
}

/* The game's texture decoded (in scratch): its key in a texture pack worked out, it written out (dumping),
 * and uploaded - or the pack's image in its place */
static void upload_pack(Texture *t, const uint32_t *reg, int w, int h) {
    int uw = w, uh = h;
    t->pending = 0;
    if (texpack_active()) {
        texpack_key(reg, texpack_draw_v, &t->pack_key, &t->pack_hash);
            texpack_dump(t->pack_key, t->pack_hash, scratch, w, h);
        pack_image(t, w, h, &uw, &uh);
    }
    upload(t, uw, uh);
}

/* A texture whose pack image was being read: uploaded once it is in */
static void pack_arrived(Texture *t, int w, int h) {
    int iw, ih;
    if (pack_image(t, w, h, &iw, &ih))
        upload(t, iw, ih);
}

/* The current texture's slot: a render target's, or a cached texture's (made or updated as needed) */
uint32_t texture_slot(const uint32_t *reg, int *w_out, int *h_out) {
    int w = 1 << (reg[0xB8] & 0xF), h = 1 << ((reg[0xB8] >> 8) & 0xF);
    *w_out = w;
    *h_out = h;
    uint32_t addr = (reg[0xA0] & 0xFFFFF0) | ((reg[0xA8] & 0x0F0000) << 8);
    int fmt = reg[0xC3] & 0xF;
    uint32_t bw = reg[0xA8] & 0x7FF;
    int filt = ((reg[0xC6] >> 8) & 1) || (reg[0xC6] & 1);
    int sampler = filt | ((reg[0xC7] & 1) << 1) | (((reg[0xC7] >> 8) & 1) << 2);

    Target *rt = (addr & 0x0F000000u) == 0x04000000u ? target_at(addr, 0) : NULL;
    if (rt) {                       /* render to texture: its size in PSP pixels is the target's */
        *w_out = *h_out = RT_SIZE;
        (void)w; (void)h;
        return rt->sample_slot;
    }

    uint64_t key = (uint64_t)addr | (uint64_t)fmt << 32 | (uint64_t)(reg[0xB8] & 0xFFFF) << 36 |
                   (uint64_t)sampler << 52 | (uint64_t)(reg[0xC2] & 1) << 55 | (uint64_t)bw << 56;
    int bits = fmt == 3 || fmt == 7 ? 32 : fmt == 4 ? 4 : fmt == 5 ? 8 : 16;
    size_t bytes = (size_t)(bw ? bw : (uint32_t)w) * (size_t)h * (size_t)bits / 8;
    /* consecutive draws mostly use the same texture: the last one found is tried first */
    static int last_found = -1;
    Texture *t = NULL;
    if (last_found >= 0 && last_found < ntextures && textures[last_found].key == key)
        t = &textures[last_found];
    else {
        /* (a table of where each key was last found, checked, before going through them all: draws change texture often) */
        static int found_at[4096];
        size_t cell = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 52);
        int i = found_at[cell];
        if (i < ntextures && textures[i].key == key)
            t = &textures[i];
        else
            for (i = 0; i < ntextures; i++)
                if (textures[i].key == key) {
                    t = &textures[i];
                    found_at[cell] = i;
                    break;
                }
        if (t)
            last_found = (int)(t - textures);
    }
    if (t && t->checked == frame_no) {
        t->used = frame_no;
        return t->slot;
    }
    if (t && t->pending)
        pack_arrived(t, w, h);
    uint64_t hash = hash_bytes(MEM(addr), bytes, 0xCBF29CE484222325ull);
    if (fmt >= 4 && fmt <= 7) {     /* the palette too */
        uint32_t caddr = (reg[0xB0] & 0xFFFFF0) | ((reg[0xB1] & 0x0F0000) << 8);
        hash = hash_bytes(MEM(caddr), (reg[0xC4] & 0x3F) * 32, hash) ^ reg[0xC5] * 0x9E3779B97F4A7C15ull;
    }
    if (t && t->hash == hash) {
        t->checked = t->used = frame_no;
        return t->slot;
    }
    if (!t) {
        if (ntextures >= MAX_TEXTURES - MAX_TARGETS - 1) {
            /* evict the least recently used */
            int lru = 0;
            for (int i = 1; i < ntextures; i++)
                if (textures[i].used < textures[lru].used)
                    lru = i;
            t = &textures[lru];
            retire(&t->img);
        } else {
            t = &textures[ntextures++];
            t->slot = next_slot++;
        }
        t->key = key;
        t->w = t->h = 0;
    }
    /* decode and upload */
    size_t size = (size_t)w * (size_t)h * 4;
    if (staging_used + size > STAGING_BYTES) {
        fprintf(stderr, "renderer: out of staging space this frame\n");
        return t->slot;
    }
    gpu_decode_texture(scratch_for(size), w, h, reg);
    if (t->slot < MAX_TEXTURES) {      /* its average colour (by alpha), for the lights effects make */
        const uint32_t *px = scratch;
        double sum[3] = {0, 0, 0}, wsum = 0;
        int step = w * h > 4096 ? (w * h) / 4096 : 1, n = 0, holes = 0;
        for (int i = 0; i < w * h; i += step) {
            uint32_t c = px[i];
            double a = (double)(c >> 24) / 255.0;
            n++;
            holes += (c >> 24) < 128;
            sum[0] += a * (c & 255);
            sum[1] += a * ((c >> 8) & 255);
            sum[2] += a * ((c >> 16) & 255);
            wsum += a;
        }
        for (int k = 0; k < 3; k++)
            tex_average[t->slot][k] = wsum > 0 ? (float)(sum[k] / wsum / 255.0) : 0;
        tex_cutout[t->slot] = holes * 50 > n;
        if (grass_made) {               /* a small copy (the ground's colour, for the grass) */
            int sw = w < 64 ? w : 64, sh = h < 64 ? h : 64;
            if (!tex_small[t->slot])
                tex_small[t->slot] = malloc(64 * 64 * 4);
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++)
                    tex_small[t->slot][y * sw + x] = px[(y * h / sh) * w + x * w / sw];
            tex_small_w[t->slot] = (uint8_t)sw;
            tex_small_h[t->slot] = (uint8_t)sh;
        }
        if (tex_holes) {                /* where the holes are */
            uint32_t cnt[HOLE_GRID * HOLE_GRID] = {0}, hole[HOLE_GRID * HOLE_GRID] = {0};
            uint32_t rgb[HOLE_GRID * HOLE_GRID][3] = {{0}};
            int sx = w > 64 ? w / 64 : 1, sy = h > 64 ? h / 64 : 1;
            for (int y = 0; y < h; y += sy)
                for (int x = 0; x < w; x += sx) {
                    int cell = (y * HOLE_GRID / h) * HOLE_GRID + x * HOLE_GRID / w;
                    uint32_t c = px[y * w + x];
                    cnt[cell]++;
                    if ((c >> 24) < 128) {
                        hole[cell]++;
                    } else {
                        rgb[cell][0] += c & 255;
                        rgb[cell][1] += (c >> 8) & 255;
                        rgb[cell][2] += (c >> 16) & 255;
                    }
                }
            for (int c = 0; c < HOLE_GRID * HOLE_GRID; c++) {
                float k = cnt[c] > hole[c] ? 1.0f / (255.0f * (float)(cnt[c] - hole[c])) : 0;
                float r = (float)rgb[c][0] * k, g = (float)rgb[c][1] * k, b = (float)rgb[c][2] * k;
                float hi = fmaxf(r, fmaxf(g, b)), lo = fminf(r, fminf(g, b));
                /* green (however dark or pale), or olive to dry brown - saturated, and not bright (paper); not grey
                 * (metal, snow) */
                int plant = hi > 0 && ((g >= fmaxf(r, b) && hi - lo >= 0.06f * hi) ||
                                       (g >= 0.55f * r && g >= 0.9f * b && hi - lo >= 0.25f * hi && hi <= 0.75f));
                tex_holes[t->slot][c] = (uint8_t)(cnt[c] && plant ? hole[c] * 255 / cnt[c] : 0);
            }
        }
    }
    t->sampler = sampler;
    upload_pack(t, reg, w, h);
    t->hash = hash;
    t->addr = addr;
    t->checked = t->used = frame_no;
    return t->slot;
}
