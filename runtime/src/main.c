/* The recompiled game: load the EBOOT's image into the PSP's memory, start its module as the PSP's kernel would, and
 * run the threads.
 *
 * Usage: mhfu <game.iso> [--eboot game/decrypted/EBOOT.elf] [--trace]
 * MHFU_TRACE=1 also traces every kernel call. */
#include "hle.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

static void on_signal(int sig) {
    (void)sig;
    exit(0);        /* run the atexit handlers (statistics) */
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*size);
    if (fread(data, 1, *size, f) != *size) {
        perror(path);
        exit(1);
    }
    fclose(f);
    return data;
}

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* Load the ELF's PT_LOAD segment; returns the end of its memory image, sets the entry and the module's gp (from
 * .rodata.sceModuleInfo: attributes, version, name[28], gp, ...) */
static uint32_t load_eboot(const char *path, uint32_t *entry, uint32_t *gp) {
    size_t size;
    uint8_t *elf = read_file(path, &size);
    if (memcmp(elf, "\x7f" "ELF", 4)) {
        fprintf(stderr, "%s: not a decrypted EBOOT (tools/dump_modules.sh)\n", path);
        exit(1);
    }
    *entry = u32(elf + 0x18);
    uint32_t phoff = u32(elf + 0x1C), shoff = u32(elf + 0x20);
    uint16_t phnum = u16(elf + 0x2C), shnum = u16(elf + 0x30), shstrndx = u16(elf + 0x32);
    uint32_t end = 0;
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + 32 * i;
        if (u32(ph) != 1)       /* PT_LOAD */
            continue;
        uint32_t off = u32(ph + 4), vaddr = u32(ph + 8), filesz = u32(ph + 16), memsz = u32(ph + 20);
        memcpy(rt_mem + vaddr, elf + off, filesz);
        memset(rt_mem + vaddr + filesz, 0, memsz - filesz);
        if (vaddr + memsz > end)
            end = vaddr + memsz;
    }
    *gp = 0;
    const uint8_t *strtab = elf + u32(elf + shoff + 40 * shstrndx + 16);
    for (int i = 0; i < shnum; i++) {
        const uint8_t *sh = elf + shoff + 40 * i;
        if (!strcmp((const char *)strtab + u32(sh), ".rodata.sceModuleInfo"))
            *gp = u32(elf + u32(sh + 16) + 32);
    }
    free(elf);
    return end;
}

int main(int argc, char **argv) {
    const char *iso = NULL, *eboot = "game/decrypted/EBOOT.elf";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--eboot") && i + 1 < argc)
            eboot = argv[++i];
        else if (!strcmp(argv[i], "--trace"))
            hle_trace = 1;
        else
            iso = argv[i];
    }
    if (!iso) {
        fprintf(stderr, "usage: %s <game.iso> [--eboot path/to/EBOOT.elf] [--trace]\n", argv[0]);
        return 2;
    }
    if (getenv("MHFU_TRACE"))
        hle_trace = 1;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    rt_mem_init();
    uint32_t entry, gp;
    uint32_t end = load_eboot(eboot, &entry, &gp);
    fprintf(stderr, "EBOOT loaded: entry %08X, gp %08X, image to %08X\n", entry, gp, end);
    kmem_init(end);
    threads_init();
    io_init(iso);
    ge_init();
    window_init();
    hle_enabled = 1;

    /* The module's start: a thread at its entry, given the EBOOT's path as its argument */
    static const char path[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    Ctx proto = {0};
    proto.r[28] = gp;
    thread_create_main(&proto, entry, 0x20, 0x40000, sizeof path, path);
    scheduler_run();
}
