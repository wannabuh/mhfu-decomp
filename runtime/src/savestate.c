/* Save states (savestate.h): the game's memory, the runtime's state of it (the SAVED section, the arena) and its
 * threads' host stacks, copied whole at a point where every thread waits - the scheduler's idle loop - and copied back
 * there. The threads are coroutines on host stacks of their own: a thread that waits is its registers (in its ucontext,
 * in the arena) and its stack's used part, so a copy of those is the thread as it was, to be resumed where it waited.
 *
 * Every host address in what is copied - the runtime's code and variables, the arena, the stacks, the game's memory -
 * is at a fixed place in the address space (the program built without PIE, the rest mapped there; no stack protector,
 * whose canary is another in each run), so that a state written to disk is loaded by the same build in another run:
 * each slot is kept in memory and written to a file (compressed, by a thread of its own) - states/stateN.bin by the
 * saves (MHFU_STATEDIR: elsewhere), with a hash of the program, as a state from another build would not load.
 *
 * MHFU_STATE_TEST=A,B: a save at game frame A, then at frame B the game's RAM noted and the state loaded; at B again,
 * the RAM compared with what was noted (with a replay, every frame from A to B is the same again). */
#include "savestate.h"
#include "hle.h"
#include "render_queue.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

/* -- the arena ---------------------------------------------------------------------------------------------------- */

#define ARENA_AT ((uintptr_t)0x5A0000000000ull)
#define ARENA_SIZE ((size_t)1 << 32)
#define CLASSES 40                  /* blocks of 16 << class bytes, a header's 16 bytes in them */
#define HEADER 16

static uint8_t *arena;
SAVED static size_t arena_top;      /* bytes of the arena handed out */
SAVED static void *free_blocks[CLASSES];

static void arena_init(void) {
    if (arena)
        return;
    void *p = mmap((void *)ARENA_AT, ARENA_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED)
        p = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("reserving the save states' arena");
        exit(1);
    }
    arena = p;
}

static int class_of(size_t size) {
    int c = 0;
    while (((size_t)16 << c) < size + HEADER)
        c++;
    return c;
}

void *st_malloc(size_t size) {
    arena_init();
    int c = class_of(size ? size : 1);
    uint8_t *b = free_blocks[c];
    if (b) {
        free_blocks[c] = *(void **)(b + HEADER);
    } else {
        size_t bytes = (size_t)16 << c;
        if (arena_top + bytes > ARENA_SIZE)
            rt_fatal(NULL, "out of the save states' arena");
        b = arena + arena_top;
        arena_top += bytes;
    }
    *(uint32_t *)b = (uint32_t)c;
    return b + HEADER;
}

void *st_calloc(size_t n, size_t size) {
    void *p = st_malloc(n * size);
    memset(p, 0, n * size);
    return p;
}

void st_free(void *p) {
    if (!p)
        return;
    uint8_t *b = (uint8_t *)p - HEADER;
    int c = (int)*(uint32_t *)b;
    *(void **)p = free_blocks[c];
    free_blocks[c] = b;
}

void *st_realloc(void *p, size_t size) {
    if (!p)
        return st_malloc(size);
    int c = (int)*(uint32_t *)((uint8_t *)p - HEADER);
    size_t have = ((size_t)16 << c) - HEADER;
    if (size <= have)
        return p;
    void *n = st_malloc(size);
    memcpy(n, p, have);
    st_free(p);
    return n;
}

/* -- the threads' host stacks ------------------------------------------------------------------------------------- */

#define STACKS_AT ((uintptr_t)0x5B0000000000ull)
#define STACK_SLOTS 128

static uint8_t *stacks;
SAVED static uint64_t stacks_used[STACK_SLOTS / 64];

void *st_stack_alloc(void) {
    if (!stacks) {
        void *p = mmap((void *)STACKS_AT, (size_t)STACK_SLOTS * ST_STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED)
            p = mmap(NULL, (size_t)STACK_SLOTS * ST_STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
        if (p == MAP_FAILED)
            rt_fatal(NULL, "out of memory for the threads' stacks");
        stacks = p;
    }
    for (int i = 0; i < STACK_SLOTS; i++)
        if (!(stacks_used[i / 64] >> (i % 64) & 1)) {
            stacks_used[i / 64] |= 1ull << (i % 64);
            return stacks + (size_t)i * ST_STACK_SIZE;
        }
    rt_fatal(NULL, "too many threads' stacks");
}

void st_stack_free(void *stack) {
    size_t i = (size_t)((uint8_t *)stack - stacks) / ST_STACK_SIZE;
    if (i < STACK_SLOTS)
        stacks_used[i / 64] &= ~(1ull << (i % 64));
}

/* -- states ------------------------------------------------------------------------------------------------------- */

extern uint8_t __start_mhfu_state[], __stop_mhfu_state[];

static const struct { uint32_t start, size; } regions[] = {
    {SCRATCHPAD_START, SCRATCHPAD_SIZE},
    {VRAM_START, VRAM_SIZE},
    {RAM_START, RAM_SIZE},
};
#define NMEM (sizeof regions / sizeof regions[0])

typedef struct {
    void *at;
    size_t size;
    uint8_t *copy;
} Span;

typedef struct {
    int used;
    uint8_t *memory[NMEM];
    uint8_t *section;
    size_t arena_bytes;
    uint8_t *arena;
    Span *spans;
    int nspans, spans_cap;
} State;

static State states[ST_SLOTS];

static State *filling;

static void take_span(void *lo, size_t size) {
    State *s = filling;
    if (s->nspans == s->spans_cap) {
        s->spans_cap = s->spans_cap ? s->spans_cap * 2 : 64;
        s->spans = realloc(s->spans, sizeof *s->spans * (size_t)s->spans_cap);
    }
    Span *sp = &s->spans[s->nspans++];
    sp->at = lo;
    sp->size = size;
    sp->copy = malloc(size);
    memcpy(sp->copy, lo, size);
}

static void state_clear(State *s) {
    for (size_t r = 0; r < NMEM; r++)
        free(s->memory[r]);
    free(s->section);
    free(s->arena);
    for (int i = 0; i < s->nspans; i++)
        free(s->spans[i].copy);
    free(s->spans);
    memset(s, 0, sizeof *s);
}

static void writer_wait(int slot);
static void write_slot(int slot);

static void save(int slot) {
    writer_wait(slot);                  /* (its copy being written: not changed under it) */
    State *s = &states[slot];
    state_clear(s);
    for (size_t r = 0; r < NMEM; r++) {
        s->memory[r] = malloc(regions[r].size);
        memcpy(s->memory[r], rt_mem + regions[r].start, regions[r].size);
    }
    size_t section = (size_t)(__stop_mhfu_state - __start_mhfu_state);
    s->section = malloc(section);
    memcpy(s->section, __start_mhfu_state, section);
    arena_init();
    s->arena_bytes = arena_top;
    s->arena = malloc(arena_top ? arena_top : 1);
    memcpy(s->arena, arena, arena_top);
    filling = s;
    thread_stack_spans(take_span);
    filling = NULL;
    s->used = 1;
    size_t stack_bytes = 0;
    for (int i = 0; i < s->nspans; i++)
        stack_bytes += s->spans[i].size;
    fprintf(stderr, "state %d saved: %zu KB of the runtime's, %zu KB of its arena, %d stacks (%zu KB)\n", slot + 1,
            section / 1024, s->arena_bytes / 1024, s->nspans, stack_bytes / 1024);
    if (slot < ST_SLOTS - 1)            /* (the last slot is the test's: memory only) */
        write_slot(slot);
}

static void load(int slot) {
    State *s = &states[slot];
    if (!s->used)
        return;
    render_queue_sync();                /* (the renderer done with what it reads of the memory) */
    atrac_state_loading();
    for (size_t r = 0; r < NMEM; r++)
        memcpy(rt_mem + regions[r].start, s->memory[r], regions[r].size);
    memcpy(__start_mhfu_state, s->section, (size_t)(__stop_mhfu_state - __start_mhfu_state));
    memcpy(arena, s->arena, s->arena_bytes);
    for (int i = 0; i < s->nspans; i++)
        memcpy(s->spans[i].at, s->spans[i].copy, s->spans[i].size);
    audio_state_loaded();
    atrac_state_loaded();
    gpu_state_loaded();
    present_state_loaded();
    fprintf(stderr, "state %d loaded\n", slot + 1);
}

/* -- on disk ------------------------------------------------------------------------------------------------------ */

#define MAGIC "MHFUSTA1"
typedef struct {
    char magic[8];
    uint64_t build;                 /* the program's hash */
    uint64_t section, arena;        /* their sizes */
    uint32_t nspans, unused;
    int64_t saved_at;               /* (the time: seconds since 1970) */
} Header;

/* A hash of the program's file (once): a state is for the build that made it */
static uint64_t build_hash(void) {
    static uint64_t h;
    if (h)
        return h;
    h = 0xCBF29CE484222325ull;
    FILE *f = fopen("/proc/self/exe", "rb");
    if (f) {
        static uint64_t buf[1 << 15];
        size_t n;
        while ((n = fread(buf, 8, sizeof buf / 8, f)) > 0)
            for (size_t i = 0; i < n; i++)
                h = (h ^ buf[i]) * 0x100000001B3ull;
        fclose(f);
    }
    return h;
}

/* Where states go: MHFU_STATEDIR, else "states" beside the saves */
static const char *state_dir(void) {
    static char dir[4096];
    if (dir[0])
        return dir;
    const char *env = getenv("MHFU_STATEDIR"), *saves = getenv("MHFU_SAVEDIR");
    if (env)
        snprintf(dir, sizeof dir, "%s", env);
    else if (saves) {
        snprintf(dir, sizeof dir, "%s", saves);
        char *slash = strrchr(dir, '/');
        snprintf(slash ? slash + 1 : dir, sizeof dir - (size_t)(slash ? slash + 1 - dir : 0), "states");
    } else if (getenv("XDG_DATA_HOME"))
        snprintf(dir, sizeof dir, "%s/mhfu-recomp/states", getenv("XDG_DATA_HOME"));
    else
        snprintf(dir, sizeof dir, "%s/.local/share/mhfu-recomp/states", getenv("HOME") ? getenv("HOME") : ".");
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0755);
            *p = '/';
        }
    mkdir(dir, 0755);
    return dir;
}

static void state_path(int slot, char *out, size_t n) {
    snprintf(out, n, "%s/state%d.bin", state_dir(), slot + 1);
}

static int deflate_out(z_stream *z, FILE *f, const void *data, size_t size, int flush) {
    static uint8_t out[1 << 16];
    z->next_in = (Bytef *)data;
    z->avail_in = (uInt)size;
    do {
        z->next_out = out;
        z->avail_out = sizeof out;
        if (deflate(z, flush) == Z_STREAM_ERROR)
            return 0;
        size_t have = sizeof out - z->avail_out;
        if (have && fwrite(out, 1, have, f) != have)
            return 0;
    } while (z->avail_out == 0 || (flush == Z_FINISH && z->avail_in));
    return 1;
}

static int write_state(const State *s, const char *path, int64_t when) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return 0;
    Header h = {{0}, build_hash(), (uint64_t)(__stop_mhfu_state - __start_mhfu_state), s->arena_bytes,
                (uint32_t)s->nspans, 0, when};
    memcpy(h.magic, MAGIC, 8);
    int ok = fwrite(&h, sizeof h, 1, f) == 1;
    for (int i = 0; i < s->nspans && ok; i++) {
        uint64_t at[2] = {(uint64_t)(uintptr_t)s->spans[i].at, s->spans[i].size};
        ok = fwrite(at, sizeof at, 1, f) == 1;
    }
    z_stream z;
    memset(&z, 0, sizeof z);
    ok = ok && deflateInit(&z, 1) == Z_OK;
    for (size_t r = 0; r < NMEM && ok; r++)
        ok = deflate_out(&z, f, s->memory[r], regions[r].size, Z_NO_FLUSH);
    ok = ok && deflate_out(&z, f, s->section, h.section, Z_NO_FLUSH);
    ok = ok && deflate_out(&z, f, s->arena, s->arena_bytes, Z_NO_FLUSH);
    for (int i = 0; i < s->nspans && ok; i++)
        ok = deflate_out(&z, f, s->spans[i].copy, s->spans[i].size, Z_NO_FLUSH);
    ok = ok && deflate_out(&z, f, NULL, 0, Z_FINISH);
    deflateEnd(&z);
    ok = fclose(f) == 0 && ok;
    if (ok && rename(tmp, path) == 0)
        return 1;
    remove(tmp);
    return 0;
}

static int inflate_into(z_stream *z, void *dst, size_t size) {
    z->next_out = dst;
    z->avail_out = (uInt)size;
    while (z->avail_out) {
        int r = inflate(z, Z_NO_FLUSH);
        if (r != Z_OK && !(r == Z_STREAM_END && z->avail_out == 0))
            return 0;
    }
    return 1;
}

/* A state from its file, into s; 0 with a reason if none, or another build's */
static int read_state(State *s, const char *path, const char **why) {
    *why = "none saved";
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    Header h;
    *why = "not a state";
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, MAGIC, 8)) {
        fclose(f);
        return 0;
    }
    *why = "made by another build";
    if (h.build != build_hash() || h.section != (uint64_t)(__stop_mhfu_state - __start_mhfu_state)) {
        fclose(f);
        return 0;
    }
    state_clear(s);
    s->nspans = s->spans_cap = (int)h.nspans;
    s->spans = calloc(h.nspans ? h.nspans : 1, sizeof *s->spans);
    int ok = 1;
    for (uint32_t i = 0; i < h.nspans && ok; i++) {
        uint64_t at[2];
        ok = fread(at, sizeof at, 1, f) == 1;
        s->spans[i].at = (void *)(uintptr_t)at[0];
        s->spans[i].size = at[1];
        s->spans[i].copy = malloc(at[1] ? at[1] : 1);
    }
    long start = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, start, SEEK_SET);
    uint8_t *packed = malloc((size_t)(end - start) + 1);
    ok = ok && fread(packed, 1, (size_t)(end - start), f) == (size_t)(end - start);
    fclose(f);
    z_stream z;
    memset(&z, 0, sizeof z);
    z.next_in = packed;
    z.avail_in = (uInt)(end - start);
    ok = ok && inflateInit(&z) == Z_OK;
    for (size_t r = 0; r < NMEM && ok; r++) {
        s->memory[r] = malloc(regions[r].size);
        ok = inflate_into(&z, s->memory[r], regions[r].size);
    }
    s->section = malloc(h.section);
    ok = ok && inflate_into(&z, s->section, h.section);
    s->arena_bytes = h.arena;
    s->arena = malloc(h.arena ? h.arena : 1);
    ok = ok && inflate_into(&z, s->arena, h.arena);
    for (uint32_t i = 0; i < h.nspans && ok; i++)
        ok = inflate_into(&z, s->spans[i].copy, s->spans[i].size);
    inflateEnd(&z);
    free(packed);
    *why = "damaged";
    if (!ok) {
        state_clear(s);
        return 0;
    }
    s->used = 1;
    return 1;
}

/* Each slot's file written by a thread of its own, from the slot's copy in memory (left alone till it is done) */
static pthread_t writers[ST_SLOTS];
static int writing[ST_SLOTS];

typedef struct { int slot; int64_t when; } Job;

static void *writer(void *arg) {
    Job *j = arg;
    char path[4200];
    state_path(j->slot, path, sizeof path);
    if (!write_state(&states[j->slot], path, j->when))
        fprintf(stderr, "state %d: could not write %s (%s)\n", j->slot + 1, path, strerror(errno));
    free(j);
    return NULL;
}

static void writer_wait(int slot) {
    if (writing[slot]) {
        pthread_join(writers[slot], NULL);
        writing[slot] = 0;
    }
}

static void writers_wait(void) {
    for (int i = 0; i < ST_SLOTS; i++)
        writer_wait(i);
}

static void write_slot(int slot) {
    static int hooked;
    if (!hooked) {
        hooked = 1;
        atexit(writers_wait);
    }
    Job *j = malloc(sizeof *j);
    j->slot = slot;
    j->when = (int64_t)time(NULL);
    if (pthread_create(&writers[slot], NULL, writer, j) == 0)
        writing[slot] = 1;
    else
        writer(j);
}

static int want_save = -1, want_load = -1;
void window_note(const char *text);

void state_request_save(int slot) {
    if (slot >= 0 && slot < ST_SLOTS)
        want_save = slot;
}

void state_request_load(int slot) {
    if (slot >= 0 && slot < ST_SLOTS)
        want_load = slot;
}

int state_slot_used(int slot) {
    if (slot < 0 || slot >= ST_SLOTS)
        return 0;
    if (states[slot].used)
        return 1;
    char path[4200];
    struct stat st;
    state_path(slot, path, sizeof path);
    return stat(path, &st) == 0;
}

unsigned long present_frame_number(void);

/* MHFU_STATE_TEST=A,B (above) */
static void state_test(void) {
    static long a = -2, b;
    static int phase;
    static uint8_t *noted;
    if (a == -2) {
        a = -1;
        if (getenv("MHFU_STATE_TEST"))
            sscanf(getenv("MHFU_STATE_TEST"), "%ld,%ld", &a, &b);
    }
    if (a < 0)
        return;
    long f = (long)present_frame_number();
    /* each frame from A to B, a hash of each 64 KB of RAM: the first frame (and pages) the second time differs */
    #define PAGES (RAM_SIZE >> 16)
    /* (from the game's memory, 0x08800000: below it the renderer's scratch, gpu.c SCRATCH_VERTICES - redrawn frames) */
    #define GAME_PAGE ((0x08800000u - RAM_START) >> 16)
    static uint64_t *hashes;
    static long last_hashed = -1, first_bad = -1;
    static uint8_t *page0;
    if (phase >= 1 && f > a && f <= b && f != last_hashed) {
        if (!hashes)
            hashes = calloc((size_t)(b - a + 1) * PAGES, sizeof *hashes);
        last_hashed = f;
        uint64_t *h = hashes + (size_t)(f - a) * PAGES;
        for (size_t pg = GAME_PAGE; pg < PAGES; pg++) {
            const uint64_t *w = (const uint64_t *)(rt_mem + RAM_START + (pg << 16));
            uint64_t x = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < (1u << 13); i++)
                x = (x ^ w[i]) * 0x100000001B3ull;
            if (phase == 1) {
                h[pg] = x;
                if (pg == GAME_PAGE) {  /* (the first page kept whole: what differs in it is printed) */
                    if (!page0)
                        page0 = calloc((size_t)(b - a + 1), 1u << 16);
                    memcpy(page0 + (size_t)(f - a) * (1u << 16), w, 1u << 16);
                }
            } else if (h[pg] != x && first_bad < 0) {
                first_bad = f;
                if (pg == GAME_PAGE && page0) {
                    const uint8_t *was = page0 + (size_t)(f - a) * (1u << 16), *now = (const uint8_t *)w;
                    int shown = 0;
                    for (size_t i = 0; i < (1u << 16) && shown < 16; i += 4)
                        if (memcmp(was + i, now + i, 4)) {
                            uint32_t x0, x1;
                            memcpy(&x0, was + i, 4);
                            memcpy(&x1, now + i, 4);
                            fprintf(stderr, "state test: %08zX was %08X, after the load %08X\n", (size_t)0x08800000u + i, x0, x1);
                            shown++;
                        }
                }
                fprintf(stderr, "state test: first differs at frame %ld:", f);
                for (size_t q = pg; q < PAGES; q++) {
                    const uint64_t *v = (const uint64_t *)(rt_mem + RAM_START + (q << 16));
                    uint64_t y = 0xCBF29CE484222325ull;
                    for (size_t i = 0; i < (1u << 13); i++)
                        y = (y ^ v[i]) * 0x100000001B3ull;
                    if (y != h[q])
                        fprintf(stderr, " %08zX", RAM_START + (q << 16));
                }
                fprintf(stderr, "\n");
                break;
            }
        }
        if (phase == 2 && f > a && last_hashed == f && first_bad < 0 && f == b)
            fprintf(stderr, "state test: every frame to %ld the same\n", f);
    }
    if (phase == 0 && f >= a) {
        fprintf(stderr, "state test: saving at frame %ld\n", f);
        want_save = ST_SLOTS - 1;
        phase = 1;
    } else if (phase == 1 && f >= b) {
        noted = malloc(RAM_SIZE);
        memcpy(noted, rt_mem + RAM_START, RAM_SIZE);
        fprintf(stderr, "state test: frame %ld noted, loading\n", f);
        want_load = ST_SLOTS - 1;
        phase = 2;
    } else if (phase == 2 && f >= b) {
        size_t differ = 0, first = 0;
        for (size_t i = GAME_PAGE << 16; i < RAM_SIZE; i++)
            if (noted[i] != rt_mem[RAM_START + i] && !differ++)
                first = i;
        if (differ) {
            fprintf(stderr, "state test: frame %ld again: %zu bytes of RAM differ (the first at %08zX)\n", f, differ,
                    RAM_START + first);
            int shown = 0;
            for (size_t i = first; i < RAM_SIZE && shown < 40;) {
                if (noted[i] == rt_mem[RAM_START + i]) {
                    i++;
                    continue;
                }
                size_t j = i;
                while (j < RAM_SIZE && j - i < 4096 && (noted[j] != rt_mem[RAM_START + j] || (j + 4 < RAM_SIZE &&
                       memcmp(noted + j, rt_mem + RAM_START + j, 4))))
                    j++;
                uint32_t was = 0, now = 0;
                memcpy(&was, noted + (i & ~(size_t)3), 4);
                memcpy(&now, rt_mem + RAM_START + (i & ~(size_t)3), 4);
                fprintf(stderr, "  %08zX..%08zX (%zu bytes): %08X, after the load %08X\n", RAM_START + i, RAM_START + j - 1,
                        j - i, was, now);
                shown++;
                i = j;
            }
        }
        else
            fprintf(stderr, "state test: frame %ld again: RAM the same\n", f);
        phase = 3;
    }
}

/* MHFU_STATE_SAVE=frame,slot and MHFU_STATE_LOAD=frame,slot (slots from 1): a save or load at a game frame; and
 * MHFU_STATE_NOTE=frame,file: the game's RAM written to a file at a frame (to compare runs) - each once */
static void state_script(void) {
    static int read;
    static long save_at = -1, load_at = -1, note_at = -1;
    static int save_slot, load_slot;
    static char note_path[1024];
    if (!read) {
        read = 1;
        if (getenv("MHFU_STATE_SAVE"))
            sscanf(getenv("MHFU_STATE_SAVE"), "%ld,%d", &save_at, &save_slot);
        if (getenv("MHFU_STATE_LOAD"))
            sscanf(getenv("MHFU_STATE_LOAD"), "%ld,%d", &load_at, &load_slot);
        if (getenv("MHFU_STATE_NOTE"))
            sscanf(getenv("MHFU_STATE_NOTE"), "%ld,%1023s", &note_at, note_path);
    }
    long f = (long)present_frame_number();
    if (save_at >= 0 && f >= save_at) {
        want_save = save_slot - 1;
        save_at = -1;
    }
    if (load_at >= 0 && f >= load_at) {
        want_load = load_slot - 1;
        load_at = -1;
    }
    if (note_at >= 0 && f >= note_at) {
        FILE *o = fopen(note_path, "wb");
        if (o) {
            fwrite(rt_mem + RAM_START, 1, RAM_SIZE, o);
            fclose(o);
        }
        fprintf(stderr, "state note: frame %ld's RAM to %s\n", f, note_path);
        note_at = -1;
    }
    /* MHFU_RAM_HASH=N: a hash of the game's RAM every N frames, printed (tools/suite.py: where two runs part) */
    static long hash_every = -1, hashed = -1;
    if (hash_every < 0)
        hash_every = getenv("MHFU_RAM_HASH") ? atol(getenv("MHFU_RAM_HASH")) : 0;
    if (hash_every > 0 && f % hash_every == 0 && f != hashed) {
        hashed = f;
        const uint64_t *w = (const uint64_t *)(rt_mem + 0x08800000u);   /* (the game's: not the renderer's scratch) */
        uint64_t x = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < (RAM_START + RAM_SIZE - 0x08800000u) / 8; i++)
            x = (x ^ w[i]) * 0x100000001B3ull;
        fprintf(stderr, "ram hash %ld %016llx\n", f, (unsigned long long)x);
    }
}

int state_point(void) {
    state_test();
    state_script();
    char note[64];
    if (want_save >= 0) {
        save(want_save);
        snprintf(note, sizeof note, "state %d saved", want_save + 1);
        window_note(note);
        want_save = -1;
    }
    if (want_load >= 0) {
        int slot = want_load;
        want_load = -1;
        const char *why = "none saved";
        char path[4200];
        state_path(slot, path, sizeof path);
        if (states[slot].used || read_state(&states[slot], path, &why)) {
            load(slot);
            snprintf(note, sizeof note, "state %d loaded", slot + 1);
            window_note(note);
            return 1;
        }
        snprintf(note, sizeof note, "state %d: %s", slot + 1, why);
        window_note(note);
        fprintf(stderr, "%s\n", note);
    }
    return 0;
}
