/* What the runtime uses of POSIX and Windows lacks, for `make WINDOWS=1`: included ahead of every file there. The rest is
 * in win_compat.c, sys/mman.h and ucontext.h; winpthreads gives pthreads, nanosleep and clock_gettime. */
#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H

#include <direct.h>
#include <io.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

/* (no permissions to give) */
static inline int compat_mkdir(const char *path) { return _mkdir(path); }
#define mkdir(path, mode) compat_mkdir(path)

static inline int compat_setenv(const char *name, const char *value, int overwrite) {
    return !overwrite && getenv(name) ? 0 : _putenv_s(name, value);
}
#define setenv(name, value, overwrite) compat_setenv(name, value, overwrite)

static inline struct tm *compat_localtime_r(const time_t *t, struct tm *out) {
    return localtime_s(out, t) ? NULL : out;
}
#define localtime_r(t, out) compat_localtime_r(t, out)

/* (what this returns may not go to free(): the one use, the render queue's ring, is never freed) */
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)

#define _SC_NPROCESSORS_ONLN 84
long sysconf(int name);

#define pipe(fds) _pipe(fds, 65536, 0x8000 /* _O_BINARY */)

/* A stream into memory: a temporary file, read back when it is closed (the captures: capture.c) */
FILE *open_memstream(char **out, size_t *size);
int compat_fclose(FILE *f);
#define fclose(f) compat_fclose(f)

#endif
