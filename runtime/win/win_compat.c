/* The Windows side of win_compat.h, sys/mman.h and ucontext.h */
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <windows.h>
#include <timeapi.h>

#include "stall.h"

#undef fclose

/* The game's files are found from the repository's top (game/...). Started anywhere else, as by a double click on the
 * program, go there: the program is at build\win\mhfu.exe. */
static void go_to_repository(void) {
    if (GetFileAttributesA("game\\decrypted\\EBOOT.elf") != INVALID_FILE_ATTRIBUTES)
        return;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    if (n == 0 || n >= sizeof path)
        return;
    for (int up = 0; up < 3; up++) {
        char *slash = strrchr(path, '\\');
        if (!slash)
            return;
        *slash = 0;
    }
    SetCurrentDirectoryA(path);
}

/* Before main: files are bytes as on Linux, not text with its line ends changed; sleeps as fine as Windows gives them;
 * what is kept under XDG_CONFIG_HOME and XDG_DATA_HOME (settings, saves, states) goes to %APPDATA%\mhfu-recomp. */
__attribute__((constructor)) static void win_start(void) {
    _set_fmode(_O_BINARY);
    go_to_repository();
    timeBeginPeriod(1);
    const char *profile = getenv("USERPROFILE"), *appdata = getenv("APPDATA");
    if (!getenv("HOME") && profile)
        _putenv_s("HOME", profile);
    if (appdata) {
        if (!getenv("XDG_CONFIG_HOME"))
            _putenv_s("XDG_CONFIG_HOME", appdata);
        if (!getenv("XDG_DATA_HOME"))
            _putenv_s("XDG_DATA_HOME", appdata);
    }
}

long sysconf(int name) {
    return name == _SC_NPROCESSORS_ONLN ? (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) : -1;
}

/* -- memory ------------------------------------------------------------------------------------------------------- */

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long long offset) {
    (void)offset;
    if (fd != -1)
        return MAP_FAILED;
    /* Address space only unless it is asked for whole: MAP_NORESERVE memory is committed by win_commit before it is used */
    DWORD type = prot != PROT_NONE && !(flags & MAP_NORESERVE) ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE;
    void *p = VirtualAlloc(addr, length, type, prot == PROT_NONE ? PAGE_NOACCESS : PAGE_READWRITE);
    return p ? p : MAP_FAILED;
}

int munmap(void *addr, size_t length) {
    (void)length;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

int mprotect(void *addr, size_t length, int prot) {
    if (prot == PROT_NONE) {
        DWORD old;
        return VirtualProtect(addr, length, PAGE_NOACCESS, &old) ? 0 : -1;
    }
    return VirtualAlloc(addr, length, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
}

int win_commit(void *addr, size_t length) {
    return VirtualAlloc(addr, length, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
}

/* -- coroutines: fibers ------------------------------------------------------------------------------------------- */

static void CALLBACK fiber_main(void *arg) {
    ((ucontext_t *)arg)->entry();
    ExitProcess(1);     /* (a coroutine ends by setcontext: thread.c) */
}

static void *own_fiber(void) {
    if (!IsThreadAFiber())
        ConvertThreadToFiber(NULL);
    return GetCurrentFiber();
}

int getcontext(ucontext_t *uc) {
    (void)uc;
    return 0;
}

void makecontext(ucontext_t *uc, void (*entry)(void), int argc, ...) {
    (void)argc;
    own_fiber();
    /* The context's stack was for a thread that has ended, which is never the one running */
    if (uc->fiber && uc->fiber != GetCurrentFiber())
        DeleteFiber(uc->fiber);
    uc->entry = entry;
    uc->fiber = CreateFiberEx(64 * 1024, uc->uc_stack.ss_size, FIBER_FLAG_FLOAT_SWITCH, fiber_main, uc);
    if (!uc->fiber) {
        fprintf(stderr, "out of memory for a thread's stack\n");
        ExitProcess(1);
    }
}

int swapcontext(ucontext_t *from, const ucontext_t *to) {
    from->fiber = own_fiber();
    SwitchToFiber(to->fiber);
    return 0;
}

int setcontext(const ucontext_t *to) {
    SwitchToFiber(to->fiber);
    return -1;
}

/* -- open_memstream ----------------------------------------------------------------------------------------------- */

static struct {
    FILE *f;
    char **out;
    size_t *size;
} memstreams[16];

FILE *open_memstream(char **out, size_t *size) {
    FILE *f = tmpfile();
    if (!f)
        return NULL;
    for (size_t i = 0; i < sizeof memstreams / sizeof memstreams[0]; i++)
        if (!memstreams[i].f) {
            memstreams[i].f = f;
            memstreams[i].out = out;
            memstreams[i].size = size;
            *out = NULL;
            *size = 0;
            return f;
        }
    fclose(f);
    return NULL;
}

int compat_fclose(FILE *f) {
    for (size_t i = 0; f && i < sizeof memstreams / sizeof memstreams[0]; i++)
        if (memstreams[i].f == f) {
            long n = ftell(f);
            char *data = malloc(n > 0 ? (size_t)n + 1 : 1);
            rewind(f);
            size_t got = data ? fread(data, 1, n > 0 ? (size_t)n : 0, f) : 0;
            if (data)
                data[got] = 0;
            *memstreams[i].out = data;
            *memstreams[i].size = got;
            memstreams[i].f = NULL;
            break;
        }
    return fclose(f);
}

/* -- no stall recorder here yet (stall.c: signals to another thread and backtraces) -------------------------------- */

void stall_beat(unsigned long frame) { (void)frame; }
void stall_thread_named(int which) { (void)which; }
const char *stall_log_path(void);
const char *stall_log_path(void) { return NULL; }
