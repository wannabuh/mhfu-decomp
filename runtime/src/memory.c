/* The PSP address space: one reservation the size of the masked address range (1 GB), with the PSP's memory mapped
 * read-write inside it and everything else left inaccessible, so a stray access faults instead of reading zeros. */
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

static const struct { uint32_t start, size; } regions[] = {
    {SCRATCHPAD_START, SCRATCHPAD_SIZE},
    {VRAM_START, VRAM_SIZE},
    {RAM_START, RAM_SIZE},
};

uint8_t *rt_mem;

uint8_t *rt_mem_init(void) {
    uint8_t *mem = mmap(NULL, (size_t)ADDR_MASK + 1, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        perror("reserving the PSP address space");
        exit(1);
    }
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        if (mprotect(mem + regions[i].start, regions[i].size, PROT_READ | PROT_WRITE)) {
            perror("mapping PSP memory");
            exit(1);
        }
    }
    rt_mem = mem;
    return mem;
}
