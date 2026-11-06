/* The PSP threads' coroutines (thread.c) on Windows fibers, which keep stacks of their own: uc_stack gives only the size */
#ifndef WIN_UCONTEXT_H
#define WIN_UCONTEXT_H

#include <stddef.h>

typedef struct ucontext_t {
    void *fiber;
    void (*entry)(void);
    struct {
        void *ss_sp;
        size_t ss_size;
    } uc_stack;
    struct ucontext_t *uc_link;
} ucontext_t;

int getcontext(ucontext_t *uc);
void makecontext(ucontext_t *uc, void (*entry)(void), int argc, ...);
int swapcontext(ucontext_t *from, const ucontext_t *to);
int setcontext(const ucontext_t *to);

#endif
