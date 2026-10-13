/* For the HLE implementations of the imported system functions.
 *
 * Each is `void hle_<name>(Ctx *c)` (names as in nids.inc), taking its arguments from the registers by the PSP's
 * calling convention - a0-a3 then t0-t3; a 64-bit argument in an aligned register pair - and returning in v0 (v1 for
 * the high half of a 64-bit result). One not implemented logs its first call and returns 0. */
#ifndef HLE_H
#define HLE_H

#include "runtime.h"

#include <stdio.h>

#define ARG0 (c->r[4])
#define ARG1 (c->r[5])
#define ARG2 (c->r[6])
#define ARG3 (c->r[7])
#define ARG4 (c->r[8])
#define ARG5 (c->r[9])
#define ARG6 (c->r[10])
#define ARG7 (c->r[11])
#define RET(v) (c->r[2] = (uint32_t)(v))
#define RET64(v) (c->r[2] = (uint32_t)(uint64_t)(v), c->r[3] = (uint32_t)((uint64_t)(v) >> 32))

/* PSP memory: HLE code reaches it through rt_mem (the same memory as the generated code's c->mem), so that helpers
 * without a context can use the same macros */
#undef MEM
#define MEM(a) (rt_mem + ((uint32_t)(a) & ADDR_MASK))
#define PTR(a) ((void *)MEM(a))
const char *hle_string(Ctx *c, uint32_t addr);     /* a NUL-terminated string in PSP memory (or "" for 0) */

/* Set by the game's main: until then (in tests), a call to an import is an error rather than a kernel call */
extern int hle_enabled;

/* Logging: HLE_LOG is per call, for tracing; off unless MHFU_TRACE is set */
extern int hle_trace;
#define HLE_LOG(...) do { if (hle_trace) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define HLE_WARN(...) do { fprintf(stderr, "warning: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* PSP error codes used here */
#define SCE_KERNEL_ERROR_UNKNOWN_THID     0x80020198u
#define SCE_KERNEL_ERROR_UNKNOWN_SEMID    0x80020199u
#define SCE_KERNEL_ERROR_UNKNOWN_EVFID    0x8002019Au
#define SCE_KERNEL_ERROR_UNKNOWN_CBID     0x800201A1u
#define SCE_KERNEL_ERROR_UNKNOWN_UID      0x800200CBu
#define SCE_KERNEL_ERROR_NO_MEMORY        0x80020190u
#define SCE_KERNEL_ERROR_ILLEGAL_PRIORITY 0x80020193u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT     0x800201A8u
#define SCE_KERNEL_ERROR_WAIT_DELETE      0x800201B5u
#define SCE_KERNEL_ERROR_SEMA_OVF         0x800201BEu
#define SCE_KERNEL_ERROR_SEMA_ZERO        0x800201BDu
#define SCE_KERNEL_ERROR_EVF_COND         0x800201AFu
#define SCE_KERNEL_ERROR_NOT_DORMANT      0x800201A4u
#define SCE_KERNEL_ERROR_DORMANT          0x800201A2u
#define SCE_ERROR_ERRNO_FILE_NOT_FOUND    0x80010002u
#define SCE_ERROR_ERRNO_BAD_FD            0x80010009u

/* Kernel objects: a UID per object, of one type */
enum ObjType { OBJ_NONE, OBJ_THREAD, OBJ_SEMA, OBJ_EVENTFLAG, OBJ_CALLBACK, OBJ_VTIMER, OBJ_MEMBLOCK, OBJ_MODULE,
               OBJ_ALARM };
uint32_t kobj_new(enum ObjType type, void *obj);
void *kobj_get(uint32_t uid, enum ObjType type);
void kobj_free(uint32_t uid);

/* Memory: the user partition's allocator (sceKernelAllocPartitionMemory, thread stacks) */
void kmem_init(uint32_t used_end);
uint32_t kmem_alloc(uint32_t size, int type, uint32_t align, uint32_t addr);
void kmem_free(uint32_t addr);

/* Time: microseconds since boot, on the host's clock */
uint64_t time_us(void);

/* Threads (thread.c) */
void threads_init(void);
uint32_t thread_create_main(Ctx *proto, uint32_t entry, uint32_t prio, uint32_t stack_size, uint32_t argsize,
                            const void *argp);
void scheduler_run(void);                       /* never returns */
Ctx *thread_ctx(void);                          /* the current thread's context */
void hle_after_call(Ctx *c);                    /* after every HLE call: due interrupts, rescheduling */
void interrupt_call(uint32_t handler, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t t0, uint32_t t1,
                    uint32_t t2);
uint32_t kernel_gp(void);
void thread_delay_until(uint64_t us);
void callback_notify(uint32_t uid, uint32_t arg);  /* a kernel callback, to run on its thread''s next ...CB wait */           /* block the current thread until then */

/* Interrupts other modules raise (ge.c, display) */
void intr_register(int intr, int sub, uint32_t handler, uint32_t arg);
#define PSP_VBLANK_INT 30
#define PSP_GE_INT 25

/* I/O (io.c) */
void io_init(const char *iso_path);

/* GE (ge.c) */
void ge_init(void);
void ge_process(void);                          /* run the queued display lists as far as they go */
void gpu_command(uint32_t cmd, uint32_t addr);  /* the renderer: one command, its data as an address */

/* The display (system.c), and frames written out (present.c) */
extern uint32_t display_fb_addr, display_fb_width, display_fb_format;
void present_frame(void);
void window_init(void);                         /* the window, and the renderer (window.c) */

#endif
