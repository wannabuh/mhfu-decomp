/* mmap and mprotect on VirtualAlloc, for what the runtime asks of them (win_compat.c): anonymous memory only */
#ifndef WIN_SYS_MMAN_H
#define WIN_SYS_MMAN_H

#include <stddef.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2

#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_NORESERVE 0x4000
#define MAP_STACK 0x20000
#define MAP_FIXED_NOREPLACE 0x100000     /* (VirtualAlloc never replaces what is there) */
#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long long offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t length, int prot);

/* MAP_NORESERVE memory is address space here until this: Linux gives it pages as they are touched */
int win_commit(void *addr, size_t length);

#endif
