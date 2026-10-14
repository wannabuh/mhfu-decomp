/* The kernel's bookkeeping - object UIDs, the user memory partition - the dispatch of imported functions to their HLE
 * implementations, and the small kernel libraries (memory, modules, utilities, power, stdio). */
#include "hle.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

int hle_trace;

const char *hle_string(Ctx *c, uint32_t addr) {
    (void)c;
    return addr ? (const char *)MEM(addr) : "";
}

/* -- objects ------------------------------------------------------------------------------------------------------ */

#define MAX_OBJECTS 4096
static struct { enum ObjType type; void *obj; } objects[MAX_OBJECTS];
#define UID_BASE 0x100

uint32_t kobj_new(enum ObjType type, void *obj) {
    for (int i = 1; i < MAX_OBJECTS; i++)
        if (objects[i].type == OBJ_NONE) {
            objects[i].type = type;
            objects[i].obj = obj;
            return UID_BASE + (uint32_t)i;
        }
    rt_fatal(NULL, "out of kernel objects");
}

void *kobj_get(uint32_t uid, enum ObjType type) {
    uint32_t i = uid - UID_BASE;
    if (uid < UID_BASE || i >= MAX_OBJECTS || objects[i].type != type)
        return NULL;
    return objects[i].obj;
}

void kobj_free(uint32_t uid) {
    uint32_t i = uid - UID_BASE;
    if (uid >= UID_BASE && i < MAX_OBJECTS)
        objects[i].type = OBJ_NONE;
}

/* -- the user partition ------------------------------------------------------------------------------------------- */

/* Blocks, sorted by address, covering the partition: free or allocated. */
typedef struct Block { uint32_t addr, size; int used; char name[32]; struct Block *next; } Block;
static Block *blocks;
#define USER_END 0x0A000000u

void kmem_init(uint32_t used_end) {
    blocks = calloc(1, sizeof *blocks);
    blocks->addr = (used_end + 0xFF) & ~0xFFu;
    blocks->size = USER_END - blocks->addr;
}

/* type: 0 low, 1 high, 2 at addr, 3 low aligned, 4 high aligned */
uint32_t kmem_alloc(uint32_t size, int type, uint32_t align, uint32_t addr) {
    if (align < 0x100)
        align = 0x100;
    size = (size + 0xFF) & ~0xFFu;
    Block *pick = NULL;
    uint32_t at = 0;
    for (Block *b = blocks; b; b = b->next) {
        if (b->used)
            continue;
        uint32_t a;
        if (type == 2) {
            if (addr < b->addr || addr + size > b->addr + b->size)
                continue;
            a = addr;
        } else if (type == 1 || type == 4) {
            if (b->size < size)
                continue;
            a = (b->addr + b->size - size) & ~(align - 1);
            if (a < b->addr)
                continue;
        } else {
            a = (b->addr + align - 1) & ~(align - 1);
            if (a + size > b->addr + b->size)
                continue;
        }
        if (type == 1 || type == 4) {
            if (!pick || a > at) {           /* the highest */
                pick = b;
                at = a;
            }
        } else if (!pick) {
            pick = b;
            at = a;
            break;
        }
    }
    if (!pick)
        return 0;
    /* split pick into [free before][used][free after] */
    if (at > pick->addr) {
        Block *n = calloc(1, sizeof *n);
        n->addr = at;
        n->size = pick->addr + pick->size - at;
        n->next = pick->next;
        pick->size = at - pick->addr;
        pick->next = n;
        pick = n;
    }
    if (pick->size > size) {
        Block *n = calloc(1, sizeof *n);
        n->addr = pick->addr + size;
        n->size = pick->size - size;
        n->next = pick->next;
        pick->size = size;
        pick->next = n;
    }
    pick->used = 1;
    return pick->addr;
}

void kmem_free(uint32_t addr) {
    for (Block *b = blocks; b; b = b->next)
        if (b->used && b->addr == addr) {
            b->used = 0;
            break;
        }
    for (Block *b = blocks; b && b->next;) {       /* merge free neighbours */
        if (!b->used && !b->next->used) {
            Block *n = b->next;
            b->size += n->size;
            b->next = n->next;
            free(n);
        } else {
            b = b->next;
        }
    }
}

typedef struct { uint32_t uid, addr; } MemBlock;

void hle_sceKernelAllocPartitionMemory(Ctx *c) {
    uint32_t addr = kmem_alloc(ARG3, (int)ARG2, ARG2 >= 3 ? ARG4 : 0x100, ARG4);
    if (!addr) {
        RET(SCE_KERNEL_ERROR_NO_MEMORY);
        return;
    }
    MemBlock *m = calloc(1, sizeof *m);
    m->addr = addr;
    m->uid = kobj_new(OBJ_MEMBLOCK, m);
    HLE_LOG("%u=sceKernelAllocPartitionMemory(%s, %X) at %08X", m->uid, hle_string(c, ARG1), ARG3, addr);
    RET(m->uid);
}

void hle_sceKernelFreePartitionMemory(Ctx *c) {
    MemBlock *m = kobj_get(ARG0, OBJ_MEMBLOCK);
    if (!m) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_UID);
        return;
    }
    kmem_free(m->addr);
    kobj_free(m->uid);
    free(m);
    RET(0);
}

void hle_sceKernelGetBlockHeadAddr(Ctx *c) {
    MemBlock *m = kobj_get(ARG0, OBJ_MEMBLOCK);
    RET(m ? m->addr : 0);
}

void hle_sceKernelSetCompiledSdkVersion500_505(Ctx *c) { RET(0); }
void hle_sceKernelSetCompilerVersion(Ctx *c) { RET(0); }

void hle_sceKernelPrintf(Ctx *c) {
    fprintf(stderr, "game: %s\n", hle_string(c, ARG0));
    RET(0);
}

/* -- modules: the game's own, and the font library it loads from the disc (replaced by HLE) ------------------------ */

#define MODULE_GAME 0x112
#define MODULE_LOADED 0x122

void hle_sceKernelGetModuleIdByAddress(Ctx *c) { RET(MODULE_GAME); }
void hle_sceKernelGetModuleId(Ctx *c) { RET(MODULE_GAME); }
void hle_sceKernelLoadModuleByID(Ctx *c) { RET(MODULE_LOADED); }
void hle_sceKernelStartModule(Ctx *c) {
    if (ARG3)
        SW(ARG3, 0);
    RET(0);
}
void hle_sceKernelStopModule(Ctx *c) { RET(0); }
void hle_sceKernelUnloadModule(Ctx *c) { RET(0); }

void hle_sceKernelExitGame(Ctx *c) {
    (void)c;
    fprintf(stderr, "the game exited\n");
    exit(0);
}

void hle_sceKernelStopUnloadSelfModuleWithStatus(Ctx *c) { hle_sceKernelExitGame(c); }
void hle_sceKernelRegisterExitCallback(Ctx *c) { RET(0); }

/* -- utilities ---------------------------------------------------------------------------------------------------- */

void hle_sceKernelDcacheWritebackAll(Ctx *c) { RET(0); }
void hle_sceKernelDcacheWritebackRange(Ctx *c) { RET(0); }
void hle_sceKernelDcacheWritebackInvalidateAll(Ctx *c) { RET(0); }
void hle_sceKernelDcacheInvalidateRange(Ctx *c) { RET(0); }
void hle_sceKernelIcacheInvalidateAll(Ctx *c) { RET(0); }

uint64_t epoch_now_us(void);
uint64_t time_read_us(void);

void hle_sceKernelLibcTime(Ctx *c) {
    uint32_t t = (uint32_t)(epoch_now_us() / 1000000u);
    if (ARG0)
        SW(ARG0, t);
    RET(t);
}

void hle_sceKernelLibcClock(Ctx *c) { RET((uint32_t)time_read_us()); }

void hle_sceKernelLibcGettimeofday(Ctx *c) {
    uint64_t us = epoch_now_us();
    if (ARG0) {
        SW(ARG0, (uint32_t)(us / 1000000u));
        SW(ARG0 + 4, (uint32_t)(us % 1000000u));
    }
    RET(0);
}

void hle_sceKernelPowerTick(Ctx *c) { RET(0); }
void hle_sceKernelVolatileMemLock(Ctx *c) { RET(0x80000041u); }       /* not available, as when the game runs */
void hle_sceKernelVolatileMemUnlock(Ctx *c) { RET(0); }

void hle_sceKernelStdin(Ctx *c) { RET(0); }
void hle_sceKernelStdout(Ctx *c) { RET(1); }
void hle_sceKernelStderr(Ctx *c) { RET(2); }
