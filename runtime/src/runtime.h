/* The runtime's own interfaces (the generated code sees only recomp.h). */
#ifndef RUNTIME_H
#define RUNTIME_H

#include "recomp.h"

#define SCRATCHPAD_START 0x00010000u
#define SCRATCHPAD_SIZE  0x00004000u
#define VRAM_START       0x04000000u
#define VRAM_SIZE        0x00200000u
#define RAM_START        0x08000000u
#define RAM_SIZE         0x02000000u

uint8_t *rt_mem_init(void);
extern uint8_t *rt_mem;             /* the address space rt_mem_init made */

/* Overlays: which module is resident in each region - read from the overlay header at the region's base, unless set
 * (for tests) */
#define NREGIONS 5
void rt_set_resident(int region, const RecompModule *m);
const RecompModule *rt_resident(int region);
const RecompModule *rt_module_named(const char *name);
const RecompModule *rt_module_numbered(int number);
RecompFunc rt_lookup(uint32_t addr, const RecompModule **in);

/* Fatal errors in the game code (a call to no function, an unknown import...). The handler, if set, is called
 * instead of exiting - the differential tester uses it to abandon a test. */
typedef void (*FatalHandler)(const char *msg);
extern FatalHandler rt_fatal_handler;
void rt_fatal(Ctx *c, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

#endif
