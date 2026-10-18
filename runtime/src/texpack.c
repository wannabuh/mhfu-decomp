/* Texture packs, as PPSSPP has them: a directory of images named by (or listed in textures.ini against) a texture's
 * key - its address, size and palette - and a hash of its data, each drawn instead of the game's texture. A pack made
 * for PPSSPP works as it is: the keys are worked out its way (its TextureReplacer and TextureCacheCommon), down to the
 * palette bytes left over from an earlier, bigger load.
 *
 * The pack is $MHFU_TEXTURES, else ~/.local/share/mhfu-recomp/TEXTURES/ULUS10391, else PPSSPP's
 * ~/.config/ppsspp/PSP/TEXTURES/ULUS10391. Images (PNG) are read by threads of their own: a texture is drawn as the
 * game has it until its image is in. MHFU_TEXTURE_DUMP=1 writes each texture the game uses to the pack's new/ under the
 * name PPSSPP gives it, to make packs from.
 *
 * Not yet: textures.zip, DDS / KTX2 / ZIM images, [indexed] textures, the mipmaps a pack gives (they are made here). */
#include "texpack.h"
#include "hle.h"

#include <ctype.h>
#include <errno.h>
#include <png.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#define GAME_ID "ULUS10391"

enum { HASH_QUICK, HASH_XXH32, HASH_XXH64 };

typedef struct {
    uint64_t cachekey;
    uint32_t hash;
    int level;
    char *file;                     /* "" marks a texture not to replace */
} Alias;

typedef struct { uint64_t cachekey; uint32_t hash; int mode; } Filter;
typedef struct { uint32_t addr, w, h, nw, nh; } HashRange;
typedef struct { uint32_t w, h; float value; } ReduceRange;

static char root[1024];
static int on, dumping;
static int hash_kind = HASH_QUICK, ignore_address, reduce_hash;
static float reduce_global = 0.5f;
static Alias *aliases;
static int naliases, alias_cap;
static Filter *filters;
static int nfilters;
static HashRange *ranges;
static int nranges;
static ReduceRange *reduces;
static int nreduces;

/* -- the hashes, as PPSSPP's ------------------------------------------------------------------------------------- */

/* StableQuickTexHash (QuickTexHashNonSSE, the same as its SSE and NEON ones) */
static uint32_t quick_hash(const uint8_t *p, uint32_t size) {
    uint32_t check = 0;
    if (!((uintptr_t)p & 0xF) && !(size & 0x3F)) {
        static const uint16_t initial[8] = {0xc00bU, 0x9bd9U, 0x4b73U, 0xb651U, 0x4d9bU, 0x4309U, 0x0083U, 0x0001U};
        uint16_t c16[8] = {0}, k[8];
        memcpy(k, initial, sizeof k);
        for (uint32_t i = 0; i < size / 16; i += 4) {
            uint16_t a[8], d[8];
            uint32_t b[4], c[4], cur[4];
            memcpy(a, p + 16 * i, 16);
            memcpy(b, p + 16 * (i + 1), 16);
            memcpy(c, p + 16 * (i + 2), 16);
            memcpy(d, p + 16 * (i + 3), 16);
            for (int j = 0; j < 8; j++)
                c16[j] = (uint16_t)(c16[j] + (uint16_t)(a[j] * k[j]));
            memcpy(cur, c16, 16);
            for (int j = 0; j < 4; j++)
                cur[j] = (cur[j] ^ b[j]) + c[j];
            memcpy(c16, cur, 16);
            for (int j = 0; j < 8; j++)
                c16[j] ^= (uint16_t)(d[j] * k[j]);
            for (int j = 0; j < 8; j++)
                k[j] = (uint16_t)(k[j] + 0x2455U);
        }
        uint32_t cur[4], kk[4];
        memcpy(cur, c16, 16);
        memcpy(kk, k, 16);
        for (int j = 0; j < 4; j++)
            cur[j] += kk[j];
        check = cur[0] + cur[1] + cur[2] + cur[3];
    } else {
        for (uint32_t i = 0; i < size / 8; i++) {
            uint32_t a, b;
            memcpy(&a, p + 8 * i, 4);
            memcpy(&b, p + 8 * i + 4, 4);
            check += a;
            check ^= b;
        }
    }
    return check;
}

static uint32_t data_hash(const uint8_t *p, uint32_t n) {
    switch (hash_kind) {
    case HASH_XXH32: return XXH32(p, n, 0xBACD7814);
    case HASH_XXH64: return (uint32_t)XXH64(p, n, 0xBACD7814);
    default: return quick_hash(p, n);
    }
}

static const uint8_t bits_per_pixel[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};
static const uint32_t align_mask[16] = {0x7F8, 0x7F8, 0x7F8, 0x7FC, 0x7E0, 0x7F0, 0x7F8, 0x7FC, 0x7FF, 0x7FF, 0x7FF};

static int find_range(uint32_t addr, int w, int h, int *nw, int *nh) {
    for (int i = 0; i < nranges; i++)
        if (ranges[i].addr == addr && ranges[i].w == (uint32_t)w && ranges[i].h == (uint32_t)h) {
            *nw = (int)ranges[i].nw;
            *nh = (int)ranges[i].nh;
            return 1;
        }
    *nw = w;
    *nh = h;
    return 0;
}

/* TextureReplacer::ComputeHash */
static uint32_t texture_hash(uint32_t addr, int bufw, int w, int h, int fmt, int max_seen_v) {
    if (!find_range(addr, w, h, &w, &h) && h == 512 && max_seen_v < 512 && max_seen_v != 0)
        h = max_seen_v;
    float reduce = 1.0f;
    if (reduce_hash) {
        reduce = reduce_global;
        for (int i = 0; i < nreduces; i++)
            if (reduces[i].w == (uint32_t)w && reduces[i].h == (uint32_t)h)
                reduce = reduces[i].value;
        reduce = reduce < 0 ? 0 : reduce > 1 ? 1 : reduce;
    }
    const uint8_t *p = MEM(addr);
    uint32_t bpp = bits_per_pixel[fmt];
    if (bufw <= w) {
        uint32_t pixels = (uint32_t)(bufw * h + (w - bufw));
        uint32_t size = (uint32_t)((float)(bpp * pixels / 8) * reduce);
        return data_hash(p, size);
    }
    uint32_t line = (uint32_t)((float)(bpp * (uint32_t)w / 8) * reduce), stride = bpp * (uint32_t)bufw / 8, result = 0;
    for (int y = 0; y < h; y++, p += stride)
        result = (result * 11) ^ data_hash(p, line);
    return result;
}

int texpack_draw_v = 512;

/* -- the palette, as PPSSPP keeps it (gpu.c tells of each load) ---------------------------------------------------- */

static uint8_t clut_raw[2048];
static uint32_t clut_total, clut_max;

void texpack_clut_loaded(uint32_t addr, uint32_t bytes) {
    if (!bytes)
        return;
    clut_total = bytes;
    memcpy(clut_raw, MEM(addr), bytes);
    if (bytes > clut_max)
        clut_max = bytes;
}

/* -- a texture's key -------------------------------------------------------------------------------------------- */

int texpack_key(const uint32_t *reg, int max_seen_v, uint64_t *cachekey, uint32_t *hash) {
    uint32_t addr = (reg[0xA0] & 0xFFFFF0) | ((reg[0xA8] & 0x0F0000) << 8);
    int fmt = reg[0xC3] & 0xF;
    if (fmt > 10)
        fmt = 0;
    int w = 1 << (reg[0xB8] & 0xF), h = 1 << ((reg[0xB8] >> 8) & 0xF);
    uint32_t dim = reg[0xB8] & 0xF0F;
    uint64_t key = (uint64_t)(addr & 0x3FFFFFFF) << 32 | dim;
    if (fmt >= 4 && fmt <= 7) {
        uint32_t clutformat = 0xC5000000u | (reg[0xC5] & 0xFFFFFF);
        uint32_t base = ((clutformat >> 16) & 0x1F) << 4;
        uint32_t base_bytes = (clutformat & 3) == 3 ? base * 4 : base * 2;
        uint32_t bytes = clut_total + base_bytes < clut_max ? clut_total + base_bytes : clut_max;
        key ^= XXH32(clut_raw, bytes, 0xC0108888) ^ clutformat;
    }
    uint32_t bufw = reg[0xA8] & align_mask[fmt];
    if (!bufw && fmt <= 10)
        bufw = 128 / bits_per_pixel[fmt];
    *cachekey = key;
    *hash = texture_hash(addr, (int)bufw, w, h, fmt, max_seen_v);
    return 1;
}

/* -- the pack's list --------------------------------------------------------------------------------------------- */

static int cmp_alias(const void *a, const void *b) {
    const Alias *x = a, *y = b;
    if (x->cachekey != y->cachekey) return x->cachekey < y->cachekey ? -1 : 1;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return x->level - y->level;
}

static void add_alias(uint64_t cachekey, uint32_t hash, int level, const char *file) {
    for (int i = 0; i < naliases; i++)       /* (the ini's word over a file's name: both are added, the ini last) */
        if (aliases[i].cachekey == cachekey && aliases[i].hash == hash && aliases[i].level == level) {
            free(aliases[i].file);
            aliases[i].file = strdup(file);
            return;
        }
    if (naliases == alias_cap) {
        alias_cap = alias_cap ? alias_cap * 2 : 1024;
        aliases = realloc(aliases, sizeof *aliases * (size_t)alias_cap);
    }
    aliases[naliases++] = (Alias){cachekey, hash, level, strdup(file)};
}

static const Alias *alias_exact(uint64_t cachekey, uint32_t hash) {
    Alias k = {cachekey, hash, 0, NULL};
    return bsearch(&k, aliases, (size_t)naliases, sizeof *aliases, cmp_alias);
}

/* LookupWildcard: the key, then with parts of it zeroed, in PPSSPP's order */
static const Alias *alias_find(uint64_t cachekey, uint32_t hash) {
    const Alias *a;
    if ((a = alias_exact(cachekey, hash))) return a;
    if ((a = alias_exact(cachekey & 0xFFFFFFFFull, 0))) return a;
    if (!ignore_address && (a = alias_exact(cachekey, 0))) return a;
    if ((a = alias_exact(cachekey & 0xFFFFFFFFull, hash))) return a;
    if (!ignore_address && (a = alias_exact(cachekey & ~0xFFFFFFFFull, hash))) return a;
    if (!ignore_address && (a = alias_exact(cachekey & ~0xFFFFFFFFull, 0))) return a;
    return alias_exact(0, hash);
}

static int filter_find(uint64_t cachekey, uint32_t hash) {
    const uint64_t keys[7] = {cachekey, cachekey & 0xFFFFFFFFull, cachekey, cachekey & 0xFFFFFFFFull,
                              cachekey & ~0xFFFFFFFFull, cachekey & ~0xFFFFFFFFull, 0};
    const uint32_t hashes[7] = {hash, 0, 0, hash, hash, 0, hash};
    for (int k = 0; k < 7; k++) {
        if (ignore_address && (k == 2 || k == 4 || k == 5))
            continue;
        for (int i = 0; i < nfilters; i++)
            if (filters[i].cachekey == keys[k] && filters[i].hash == hashes[k])
                return filters[i].mode;
    }
    for (int i = 0; i < nfilters; i++)
        if (!filters[i].cachekey && !filters[i].hash)
            return filters[i].mode;
    return TEXPACK_FILTER_GAME;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static int is_image(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && !strcasecmp(dot, ".png");
}

/* Images in the pack's root named by their key (ScanForHashNamedFiles) */
static void scan_root(void) {
    DIR *d = opendir(root);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !is_image(e->d_name))
            continue;
        size_t n = strlen(e->d_name) - 4;
        if (!(n == 24 || ((n == 26 || n == 27) && e->d_name[24] == '_')))
            continue;
        unsigned long long ck = 0;
        unsigned h = 0;
        int level = 0;
        if (sscanf(e->d_name, "%16llx%8x_%d", &ck, &h, &level) >= 1)
            add_alias(ck, h, level, e->d_name);
    }
    closedir(d);
}

static int load_ini(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/textures.ini", root);
    FILE *f = fopen(path, "r");
    if (!f) {
        scan_root();
        return naliases > 0;
    }
    scan_root();
    char line[2048], section[64] = "";
    int have_hash = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#')
            continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) *e = 0;
            snprintf(section, sizeof section, "%s", s + 1);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (!strcasecmp(section, "options")) {
            int t = !strcasecmp(v, "true") || !strcmp(v, "1");
            if (!strcasecmp(k, "hash")) {
                have_hash = 1;
                hash_kind = !strcasecmp(v, "xxh32") ? HASH_XXH32 : !strcasecmp(v, "xxh64") ? HASH_XXH64 : HASH_QUICK;
                if (hash_kind == HASH_QUICK && strcasecmp(v, "quick"))
                    fprintf(stderr, "textures: unsupported hash type %s\n", v);
            } else if (!strcasecmp(k, "ignoreAddress")) {
                ignore_address = t;
            } else if (!strcasecmp(k, "reduceHash")) {
                reduce_hash = t;
            }
        } else if (!strcasecmp(section, "hashes")) {
            unsigned long long ck = 0;
            unsigned h = 0;
            int level = 0;
            if (strstr(v, "..") || sscanf(k, "%16llx%8x_%d", &ck, &h, &level) < 1)
                continue;
            for (char *c = v; *c; c++)
                if (*c == '\\') *c = '/';
            add_alias(ck, h, level, v);
        } else if (!strcasecmp(section, "hashranges")) {
            unsigned a, w, h, nw, nh;
            if (sscanf(k, "%x,%u,%u", &a, &w, &h) == 3 && sscanf(v, "%u,%u", &nw, &nh) == 2 && nw && nh && nw <= w &&
                nh <= h) {
                ranges = realloc(ranges, sizeof *ranges * (size_t)(nranges + 1));
                ranges[nranges++] = (HashRange){a, w, h, nw, nh};
            }
        } else if (!strcasecmp(section, "filtering")) {
            unsigned long long ck = 0;
            unsigned h = 0;
            int mode = !strcasecmp(v, "nearest") ? TEXPACK_FILTER_NEAREST : !strcasecmp(v, "linear") ? TEXPACK_FILTER_LINEAR
                       : TEXPACK_FILTER_GAME;
            if (sscanf(k, "%16llx%8x", &ck, &h) >= 1) {
                filters = realloc(filters, sizeof *filters * (size_t)(nfilters + 1));
                filters[nfilters++] = (Filter){ck, h, mode};
            }
        } else if (!strcasecmp(section, "reducehashranges")) {
            unsigned w, h;
            float r;
            if (sscanf(k, "%u,%u", &w, &h) == 2 && sscanf(v, "%f", &r) == 1 && r != 0) {
                reduces = realloc(reduces, sizeof *reduces * (size_t)(nreduces + 1));
                reduces[nreduces++] = (ReduceRange){w, h, r};
            }
        }
    }
    fclose(f);
    if (!have_hash) {
        fprintf(stderr, "textures: %s names no hash type\n", path);
        return 0;
    }
    if (reduce_hash && hash_kind == HASH_QUICK)
        reduce_hash = 0;
    if (ignore_address && hash_kind == HASH_QUICK)
        ignore_address = 0;
    return 1;
}

/* -- images, read by a thread of their own ------------------------------------------------------------------------ */

typedef struct {
    char *file;
    int state;                      /* 0 asked for, 1 reading, 2 in, 3 failed, 4 taken */
    uint32_t *pixels;
    int w, h;
} Image;

static Image *images;
static int nimages, image_cap;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

static uint32_t *read_png(const char *path, int *w, int *h) {
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path))
        return NULL;
    img.format = PNG_FORMAT_RGBA;
    uint32_t *px = malloc(PNG_IMAGE_SIZE(img));
    if (!px || !png_image_finish_read(&img, NULL, px, 0, NULL)) {
        free(px);
        png_image_free(&img);
        return NULL;
    }
    *w = (int)img.width;
    *h = (int)img.height;
    return px;
}

static void *reader(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    for (;;) {
        int i;
        for (i = 0; i < nimages && images[i].state != 0; i++)
            ;
        if (i == nimages) {
            pthread_cond_wait(&wake, &lock);
            continue;
        }
        images[i].state = 1;
        char path[2200];
        snprintf(path, sizeof path, "%s/%s", root, images[i].file);
        pthread_mutex_unlock(&lock);
        int w = 0, h = 0;
        uint32_t *px = read_png(path, &w, &h);
        if (!px)
            fprintf(stderr, "textures: can't read %s\n", path);
        pthread_mutex_lock(&lock);
        images[i].pixels = px;
        images[i].w = w;
        images[i].h = h;
        images[i].state = px ? 2 : 3;
    }
    return NULL;
}

/* The image of a pack's file: 1 with it (the caller's to free), 0 not read yet (asked for), -1 never */
static int image_take(const char *file, uint32_t **px, int *w, int *h) {
    pthread_mutex_lock(&lock);
    int i;
    for (i = 0; i < nimages && strcmp(images[i].file, file); i++)
        ;
    if (i == nimages) {
        if (nimages == image_cap) {
            image_cap = image_cap ? image_cap * 2 : 256;
            images = realloc(images, sizeof *images * (size_t)image_cap);
        }
        images[nimages++] = (Image){strdup(file), 0, NULL, 0, 0};
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&lock);
        return 0;
    }
    Image *m = &images[i];
    int r = 0;
    if (m->state == 2) {
        *px = m->pixels;
        *w = m->w;
        *h = m->h;
        m->pixels = NULL;
        m->state = 4;               /* taken: read again if asked for again (its texture let go) */
        r = 1;
    } else if (m->state == 4) {
        m->state = 0;
        pthread_cond_signal(&wake);
    } else if (m->state == 3) {
        r = -1;
    }
    pthread_mutex_unlock(&lock);
    return r;
}

/* -- the renderer's side ------------------------------------------------------------------------------------------ */

static const char *pack_dir(void) {
    static char path[1024];
    const char *e = getenv("MHFU_TEXTURES"), *home = getenv("HOME");
    struct stat st;
    if (e && *e) {
        if (e[0] == '~' && home)
            snprintf(path, sizeof path, "%s%s", home, e + 1);
        else
            snprintf(path, sizeof path, "%s", e);
        return path;
    }
    if (!home)
        return NULL;
    snprintf(path, sizeof path, "%s/.local/share/mhfu-recomp/TEXTURES/" GAME_ID, home);
    if (!stat(path, &st) && S_ISDIR(st.st_mode))
        return path;
    snprintf(path, sizeof path, "%s/.config/ppsspp/PSP/TEXTURES/" GAME_ID, home);
    if (!stat(path, &st) && S_ISDIR(st.st_mode))
        return path;
    if (getenv("MHFU_TEXTURE_DUMP")) {
        snprintf(path, sizeof path, "%s/.local/share/mhfu-recomp/TEXTURES/" GAME_ID, home);
        return path;
    }
    return NULL;
}

static void make_dirs(const char *path) {
    char p[1100];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir(p, 0755);
            *c = '/';
        }
    mkdir(p, 0755);
}

void texpack_init(void) {
    const char *dir = pack_dir();
    dumping = getenv("MHFU_TEXTURE_DUMP") && atoi(getenv("MHFU_TEXTURE_DUMP"));
    if (!dir)
        return;
    snprintf(root, sizeof root, "%s", dir);
    if (dumping) {
        char p[1100];
        snprintf(p, sizeof p, "%s/new", root);
        make_dirs(p);
        fprintf(stderr, "textures: writing the game's to %s\n", p);
    }
    if (!load_ini())
        return;
    qsort(aliases, (size_t)naliases, sizeof *aliases, cmp_alias);
    on = 1;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int readers = cpus > 8 ? 4 : cpus > 2 ? (int)cpus / 2 : 1;
    for (int i = 0; i < readers; i++) {
        pthread_t t;
        pthread_create(&t, NULL, reader, NULL);
        pthread_detach(t);
    }
    int images_named = 0;
    for (int i = 0; i < naliases; i++)
        images_named += aliases[i].level == 0 && aliases[i].file[0];
    fprintf(stderr, "textures: pack %s, %d images (%s hash)\n", root, images_named,
            hash_kind == HASH_XXH32 ? "xxh32" : hash_kind == HASH_XXH64 ? "xxh64" : "quick");
}

int texpack_active(void) { return on || dumping; }

/* The file a key's texture is replaced with, or NULL */
static const char *replacement(uint64_t cachekey, uint32_t hash) {
    if (!on)
        return NULL;
    if (ignore_address)
        cachekey &= 0xFFFFFFFFull;
    const Alias *a = alias_find(cachekey, hash);
    if (!a || !a->file[0] || a->level != 0)
        return NULL;
    /* (an alias of mipmaps: "level0|level1|..." - the first) */
    static char first[1024];
    snprintf(first, sizeof first, "%s", a->file);
    char *bar = strchr(first, '|');
    if (bar)
        *bar = 0;
    return first[0] ? first : NULL;
}

int texpack_find(uint64_t cachekey, uint32_t hash, int w, int h, TexpackImage *out) {
    const char *file = replacement(cachekey, hash);
    if (!file)
        return -1;
    uint32_t *px = NULL;
    int iw = 0, ih = 0, r = image_take(file, &px, &iw, &ih);
    if (r <= 0)
        return r;
    /* a hash range: the image is that part of the texture (the rest of it left clear) */
    int nw, nh;
    find_range((uint32_t)(cachekey >> 32), w, h, &nw, &nh);
    out->filter = filter_find(cachekey, hash);
    if (nw == w && nh == h) {
        out->pixels = px;
        out->w = iw;
        out->h = ih;
        return 1;
    }
    int fw = iw * w / nw, fh = ih * h / nh;
    uint32_t *full = calloc((size_t)fw * (size_t)fh, 4);
    for (int y = 0; y < ih && y < fh; y++)
        memcpy(full + (size_t)y * (size_t)fw, px + (size_t)y * (size_t)iw, (size_t)(iw < fw ? iw : fw) * 4);
    free(px);
    out->pixels = full;
    out->w = fw;
    out->h = fh;
    return 1;
}

/* MHFU_TEXTURE_DUMP: the game's texture (decoded) under its key's name, once */
void texpack_dump(uint64_t cachekey, uint32_t hash, const uint32_t *px, int w, int h) {
    if (!dumping || !root[0])
        return;
    static uint64_t *done;
    static int ndone, dcap;
    uint64_t id = cachekey * 0x9E3779B97F4A7C15ull ^ hash;
    for (int i = 0; i < ndone; i++)
        if (done[i] == id)
            return;
    if (ndone == dcap) {
        dcap = dcap ? dcap * 2 : 1024;
        done = realloc(done, sizeof *done * (size_t)dcap);
    }
    done[ndone++] = id;
    char path[1200];
    snprintf(path, sizeof path, "%s/new/%016llx%08x.png", root, (unsigned long long)cachekey, hash);
    struct stat st;
    if (!stat(path, &st))
        return;
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    img.width = (png_uint_32)w;
    img.height = (png_uint_32)h;
    img.format = PNG_FORMAT_RGBA;
    if (!png_image_write_to_file(&img, path, 0, px, 0, NULL))
        fprintf(stderr, "textures: can't write %s\n", path);
}
