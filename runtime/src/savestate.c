/* Save states (savestate.h): the game's memory and the runtime's state of it (the SAVED section, the arena), copied whole
 * at a point where every thread waits - the scheduler's idle loop - and copied back there.
 *
 * The threads' host stacks are not kept: they hold addresses in the program, so no other build could use them. A thread
 * that waits is in a kernel call, with every register of the game's in its Ctx, and generated code keeps nothing else
 * across a call: after a load it finishes the call and goes on from the return address (thread.c thread_resume_main,
 * dispatch.c rt_resume_run). What cannot be left that way is a frame with C state on a thread's stack (gated code, a
 * callback, a hand-written function: rt_no_resume) - a save waits for a point with none.
 *
 * On disk (states/stateN.bin; MHFU_STATEDIR: elsewhere): a header, then named chunks compressed - the memory ("mem:..."),
 * the arena ("arena"), each SAVED variable by file and name ("var:camera.c:left", from the program's own symbol table,
 * which so must not be stripped). A later build loads it: a variable it does not have is ignored, one the state does not
 * have takes its value at boot, one of another size refuses the load and is named. The arena's structs stay as they were
 * (ST_LAYOUT and their sizes), and at its fixed address, the pointers in it good.
 *
 * MHFU_STATE_TEST=A,B: a save at game frame A, then at frame B the game's RAM noted and the state loaded; at B again,
 * the RAM compared with what was noted (with a replay, every frame from A to B is the same again). */
#include "savestate.h"
#include "hle.h"
#include "render_queue.h"

#include <elf.h>
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

SAVED int rt_no_resume;

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

#define STACK_SLOTS 128

static uint8_t *stacks;
static uint64_t stacks_used[STACK_SLOTS / 64];

void *st_stack_alloc(void) {
    if (!stacks) {
        void *p = mmap(NULL, (size_t)STACK_SLOTS * ST_STACK_SIZE, PROT_READ | PROT_WRITE,
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

void st_stacks_reset(void) {
    memset(stacks_used, 0, sizeof stacks_used);
}

/* -- the SAVED variables, by name ---------------------------------------------------------------------------------- */

extern uint8_t __start_mhfu_state[], __stop_mhfu_state[];

typedef struct {
    char name[120];                 /* "var:file.c:name"; a global, "var:name" */
    uint8_t *at;
    size_t size;
} Var;

static Var *vars;
static int nvars;
static uint8_t *section_at_boot;    /* the section's bytes in the program's file */

/* From the program's own symbol table, once. A local symbol's file is the last FILE symbol before it. */
static int vars_init(void) {
    static int done;
    if (done)
        return nvars > 0;
    done = 1;
    size_t section = (size_t)(__stop_mhfu_state - __start_mhfu_state);
    FILE *f = fopen("/proc/self/exe", "rb");
    if (!f)
        return 0;
    fseeko(f, 0, SEEK_END);
    size_t file_size = (size_t)ftello(f);
    uint8_t *e = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fileno(f), 0);
    fclose(f);
    if (e == MAP_FAILED)
        return 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)e;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(e + eh->e_shoff);
    const char *names = (const char *)e + sh[eh->e_shstrndx].sh_offset;
    size_t saved = 0, symtab = 0;
    for (size_t i = 1; i < eh->e_shnum; i++) {
        if (!strcmp(names + sh[i].sh_name, "mhfu_state"))
            saved = i;
        if (sh[i].sh_type == SHT_SYMTAB)
            symtab = i;
    }
    if (saved && symtab) {
        section_at_boot = malloc(section);
        memcpy(section_at_boot, e + sh[saved].sh_offset, section);
        const Elf64_Sym *sym = (const Elf64_Sym *)(e + sh[symtab].sh_offset);
        size_t n = sh[symtab].sh_size / sizeof *sym, covered = 0;
        const char *strs = (const char *)e + sh[sh[symtab].sh_link].sh_offset, *file = "";
        vars = malloc(n && section ? section / 4 * sizeof *vars : sizeof *vars);
        for (size_t i = 1; i < n; i++) {
            if (ELF64_ST_TYPE(sym[i].st_info) == STT_FILE)
                file = strs + sym[i].st_name;
            if (sym[i].st_shndx != saved || ELF64_ST_TYPE(sym[i].st_info) != STT_OBJECT || !sym[i].st_size)
                continue;
            Var *v = &vars[nvars++];
            if (ELF64_ST_BIND(sym[i].st_info) == STB_LOCAL)
                snprintf(v->name, sizeof v->name, "var:%s:%s", file, strs + sym[i].st_name);
            else
                snprintf(v->name, sizeof v->name, "var:%s", strs + sym[i].st_name);
            v->at = __start_mhfu_state + (sym[i].st_value - sh[saved].sh_addr);
            v->size = sym[i].st_size;
            covered += v->size;
        }
        for (int i = 0; i < nvars; i++)
            for (int j = 0; j < i; j++)
                if (!strcmp(vars[i].name, vars[j].name))
                    rt_fatal(NULL, "save states: two variables named %s", vars[i].name + 4);
        if (section - covered >= 4096)      /* (a little is padding between them) */
            fprintf(stderr, "save states: %zu bytes of the section have no name\n", section - covered);
    }
    munmap(e, file_size);
    return nvars > 0;
}

/* -- states ------------------------------------------------------------------------------------------------------- */

static const struct { const char *name; uint32_t start, size; } regions[] = {
    {"mem:scratchpad", SCRATCHPAD_START, SCRATCHPAD_SIZE},
    {"mem:vram", VRAM_START, VRAM_SIZE},
    {"mem:ram", RAM_START, RAM_SIZE},
};
#define NMEM (sizeof regions / sizeof regions[0])

typedef struct {
    char *name;
    size_t size;
    uint8_t *data;
} Chunk;

typedef struct {
    int used;
    Chunk *chunks;
    int nchunks;
    uint64_t game, layout;
} State;

static State states[ST_SLOTS];

static uint64_t state_layout(void) {
    uint64_t h = 0xCBF29CE484222325ull;
    uint64_t parts[] = {ST_LAYOUT, thread_layout(), kernel_layout()};
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++)
        h = (h ^ parts[i]) * 0x100000001B3ull;
    return h;
}

static Chunk *chunk_add(State *s, const char *name, size_t size, const void *from) {
    s->chunks = realloc(s->chunks, sizeof *s->chunks * (size_t)(s->nchunks + 1));
    Chunk *k = &s->chunks[s->nchunks++];
    k->name = strdup(name);
    k->size = size;
    k->data = malloc(size ? size : 1);
    if (from)
        memcpy(k->data, from, size);
    return k;
}

static Chunk *chunk_find(const State *s, const char *name) {
    for (int i = 0; i < s->nchunks; i++)
        if (!strcmp(s->chunks[i].name, name))
            return &s->chunks[i];
    return NULL;
}

static void state_clear(State *s) {
    for (int i = 0; i < s->nchunks; i++) {
        free(s->chunks[i].name);
        free(s->chunks[i].data);
    }
    free(s->chunks);
    memset(s, 0, sizeof *s);
}

static void writer_wait(int slot);
static void write_slot(int slot);

/* The game as it is now, into a state (empty) */
static void state_fill(State *s) {
    render_queue_sync();                /* (the renderer thread's writes to what is saved: rt_cull_margin) */
    s->game = io_game_id();
    s->layout = state_layout();
    for (size_t r = 0; r < NMEM; r++)
        chunk_add(s, regions[r].name, regions[r].size, rt_mem + regions[r].start);
    arena_init();
    chunk_add(s, "arena", arena_top, arena);
    for (int i = 0; i < nvars; i++)
        chunk_add(s, vars[i].name, vars[i].size, vars[i].at);
    s->used = 1;
}

static void save(int slot) {
    if (!vars_init()) {
        fprintf(stderr, "state %d not saved: the program has no symbol table\n", slot + 1);
        return;
    }
    writer_wait(slot);                  /* (its copy being written: not changed under it) */
    State *s = &states[slot];
    state_clear(s);
    state_fill(s);
    fprintf(stderr, "state %d saved: %d variables, %zu KB of its arena\n", slot + 1, nvars, arena_top / 1024);
    if (slot < ST_SLOTS - 1)            /* (the last slot is the test's: memory only) */
        write_slot(slot);
}

/* Whether this build can load a state; if not, why */
static int state_fits(const State *s, char *why, size_t n) {
    if (!vars_init()) {
        snprintf(why, n, "the program has no symbol table");
        return 0;
    }
    if (s->game != io_game_id()) {
        snprintf(why, n, "made with another copy of the game");
        return 0;
    }
    if (s->layout != state_layout()) {
        snprintf(why, n, "the kernel's structs have changed since");
        return 0;
    }
    arena_init();
    if ((uintptr_t)arena != ARENA_AT) {
        snprintf(why, n, "the arena is not at its place");
        return 0;
    }
    for (size_t r = 0; r < NMEM; r++) {
        const Chunk *k = chunk_find(s, regions[r].name);
        if (!k || k->size != regions[r].size) {
            snprintf(why, n, "no %s", regions[r].name);
            return 0;
        }
    }
    if (!chunk_find(s, "arena")) {
        snprintf(why, n, "no arena");
        return 0;
    }
    for (int i = 0; i < nvars; i++) {
        const Chunk *k = chunk_find(s, vars[i].name);
        if (k && k->size != vars[i].size) {
            snprintf(why, n, "%s was %zu bytes, now %zu", vars[i].name + 4, k->size, vars[i].size);
            return 0;
        }
    }
    return 1;
}

static void load(int slot) {
    const State *s = &states[slot];
    render_queue_sync();                /* (the renderer done with what it reads of the memory) */
    atrac_state_loading();
    for (size_t r = 0; r < NMEM; r++)
        memcpy(rt_mem + regions[r].start, chunk_find(s, regions[r].name)->data, regions[r].size);
    for (int i = 0; i < nvars; i++) {
        const Chunk *k = chunk_find(s, vars[i].name);
        if (k) {
            memcpy(vars[i].at, k->data, k->size);
        } else {
            fprintf(stderr, "state %d: no %s in it: as at boot\n", slot + 1, vars[i].name + 4);
            memcpy(vars[i].at, section_at_boot + (vars[i].at - __start_mhfu_state), vars[i].size);
        }
    }
    for (int i = 0; i < s->nchunks; i++)
        if (!strncmp(s->chunks[i].name, "var:", 4)) {
            int known = 0;
            for (int j = 0; j < nvars && !known; j++)
                known = !strcmp(vars[j].name, s->chunks[i].name);
            if (!known)
                fprintf(stderr, "state %d: %s is no more: left out\n", slot + 1, s->chunks[i].name + 4);
        }
    const Chunk *a = chunk_find(s, "arena");
    memcpy(arena, a->data, a->size);
    rt_no_resume = 0;
    threads_state_loaded();
    audio_state_loaded();
    atrac_state_loaded();
    gpu_state_loaded();
    present_state_loaded();
    fprintf(stderr, "state %d loaded\n", slot + 1);
}

/* -- on disk ------------------------------------------------------------------------------------------------------ */

#define MAGIC "MHFUPST1"
#define FORMAT 1
typedef struct {
    char magic[8];
    uint32_t format, nchunks;
    uint64_t game, layout;
    int64_t saved_at;               /* (the time: seconds since 1970) */
    uint64_t frame;                 /* (the game's frame, to read) */
} Header;

/* Each chunk, in the compressed stream: this, its name, its data */
typedef struct { uint32_t name, unused; uint64_t size; } ChunkHeader;

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
    static _Thread_local uint8_t out[1 << 16];  /* (each slot has a writer of its own) */
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

static int write_state(const State *s, const char *path, int64_t when, uint64_t frame) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return 0;
    Header h = {{0}, FORMAT, (uint32_t)s->nchunks, s->game, s->layout, when, frame};
    memcpy(h.magic, MAGIC, 8);
    int ok = fwrite(&h, sizeof h, 1, f) == 1;
    z_stream z;
    memset(&z, 0, sizeof z);
    ok = ok && deflateInit(&z, 1) == Z_OK;
    for (int i = 0; i < s->nchunks && ok; i++) {
        const Chunk *k = &s->chunks[i];
        ChunkHeader ch = {(uint32_t)strlen(k->name), 0, k->size};
        ok = deflate_out(&z, f, &ch, sizeof ch, Z_NO_FLUSH);
        ok = ok && deflate_out(&z, f, k->name, ch.name, Z_NO_FLUSH);
        ok = ok && deflate_out(&z, f, k->data, k->size, Z_NO_FLUSH);
    }
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

/* A state from its file, into s; 0 with a reason if none, or not one */
static int read_state(State *s, const char *path, const char **why) {
    *why = "none saved";
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    Header h;
    *why = "not a state, or from before states loaded in later builds";
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, MAGIC, 8) || h.format != FORMAT) {
        fclose(f);
        return 0;
    }
    state_clear(s);
    s->game = h.game;
    s->layout = h.layout;
    long start = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, start, SEEK_SET);
    uint8_t *packed = malloc((size_t)(end - start) + 1);
    int ok = fread(packed, 1, (size_t)(end - start), f) == (size_t)(end - start);
    fclose(f);
    z_stream z;
    memset(&z, 0, sizeof z);
    z.next_in = packed;
    z.avail_in = (uInt)(end - start);
    ok = ok && inflateInit(&z) == Z_OK;
    for (uint32_t i = 0; i < h.nchunks && ok; i++) {
        ChunkHeader ch;
        char name[256];
        ok = inflate_into(&z, &ch, sizeof ch) && ch.name < sizeof name && inflate_into(&z, name, ch.name);
        if (ok) {
            name[ch.name] = 0;
            ok = inflate_into(&z, chunk_add(s, name, ch.size, NULL)->data, ch.size);
        }
    }
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

unsigned long present_frame_number(void);

/* Each slot's file written by a thread of its own, from the slot's copy in memory (left alone till it is done) */
static pthread_t writers[ST_SLOTS];
static int writing[ST_SLOTS];

typedef struct { int slot; int64_t when; uint64_t frame; } Job;

static void *writer(void *arg) {
    Job *j = arg;
    char path[4200];
    state_path(j->slot, path, sizeof path);
    if (!write_state(&states[j->slot], path, j->when, j->frame))
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
    j->frame = present_frame_number();
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

/* A capture's state (capture.c): in a file of its own, the slots left alone */
static char capture_path[4200];
static int want_capture;
static unsigned long capture_asked_at;

void capture_state_taken(unsigned long frame, int late);
void capture_state_written(void);
uint64_t wall_time_us(void);

void state_request_capture(const char *path) {
    snprintf(capture_path, sizeof capture_path, "%s", path);
    want_capture = 1;
    capture_asked_at = present_frame_number();
}

void state_cancel_capture(void) {
    want_capture = 0;
}

typedef struct { State s; char path[4200]; int64_t when; uint64_t frame; } CaptureJob;

static void *capture_writer(void *arg) {
    CaptureJob *j = arg;
    if (!write_state(&j->s, j->path, j->when, j->frame))
        fprintf(stderr, "capture: could not write %s (%s)\n", j->path, strerror(errno));
    state_clear(&j->s);
    free(j);
    capture_state_written();
    return NULL;
}

static void take_capture(void) {
    want_capture = 0;
    CaptureJob *j = calloc(1, sizeof *j);
    if (!vars_init()) {
        fprintf(stderr, "capture: no state: the program has no symbol table\n");
        free(j);
        return;
    }
    uint64_t t0 = wall_time_us();
    state_fill(&j->s);
    fprintf(stderr, "capture: the state's copy took %.1f ms on the game's thread\n", (double)(wall_time_us() - t0) / 1000);
    snprintf(j->path, sizeof j->path, "%s", capture_path);
    j->when = (int64_t)time(NULL);
    j->frame = present_frame_number();
    capture_state_taken(present_frame_number(), (int)(present_frame_number() - capture_asked_at));
    pthread_t t;
    if (pthread_create(&t, NULL, capture_writer, j) == 0)
        pthread_detach(t);
    else
        capture_writer(j);
}

int state_point(void) {
    state_test();
    state_script();
    if (want_capture && rt_no_resume == 0)
        take_capture();
    char note[256];
    static long deferred_from = -1;
    long f = (long)present_frame_number();
    if (want_save >= 0 && rt_no_resume > 0) {   /* (a frame with C state on a thread's stack: not yet) */
        if (deferred_from < 0) {
            deferred_from = f;
            fprintf(stderr, "state %d: the save waits, at frame %ld (%d frames holding C state)\n", want_save + 1, f,
                    rt_no_resume);
        } else if (f == deferred_from + 300) {
            snprintf(note, sizeof note, "state %d: cannot be saved here yet", want_save + 1);
            window_note(note);
            fprintf(stderr, "%s\n", note);
        }
    } else if (want_save >= 0) {
        if (deferred_from >= 0)
            fprintf(stderr, "state %d: saved %ld frames late\n", want_save + 1, f - deferred_from);
        deferred_from = -1;
        save(want_save);
        snprintf(note, sizeof note, "state %d saved", want_save + 1);
        window_note(note);
        want_save = -1;
    }
    if (want_load >= 0) {
        int slot = want_load;
        want_load = -1;
        const char *why = "none saved";
        char path[4200], unfit[200];
        state_path(slot, path, sizeof path);
        if (states[slot].used || read_state(&states[slot], path, &why)) {
            if (state_fits(&states[slot], unfit, sizeof unfit)) {
                want_save = -1;         /* (one that waited: of a game no more) */
                want_capture = 0;
                deferred_from = -1;
                load(slot);
                snprintf(note, sizeof note, "state %d loaded", slot + 1);
                window_note(note);
                return 1;
            }
            why = unfit;
        }
        snprintf(note, sizeof note, "state %d: %s", slot + 1, why);
        window_note(note);
        fprintf(stderr, "%s\n", note);
    }
    return 0;
}
