/* The recompiled game: load the EBOOT's image into the PSP's memory, start its module as the PSP's kernel would, and
 * run the threads.
 *
 * Usage: mhfu [game.iso] [--eboot game/decrypted/EBOOT.elf] [--trace]
 * MHFU_TRACE=1 also traces every kernel call.
 *
 * Settings: lines "KEY = value" in ./mhfu.ini, then in $XDG_CONFIG_HOME/mhfu-recomp/settings.ini (~/.config/...), set
 * the environment variables MHFU_KEY (the prefix optional: "fps = 144" is MHFU_FPS=144) where they are not set
 * already - the environment first, then ./mhfu.ini, then settings.ini, then the defaults below. "iso = path" names the
 * game when the command line does not. Lines starting with # are comments. */
#include "hle.h"
#include "pacing.h"

extern const char *rt_settings_path;

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

static char *iso_setting;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static void load_settings(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line), *eq = strchr(l, '=');
        if (!*l || *l == '#' || !eq)
            continue;
        *eq = 0;
        char *key = trim(l), *value = trim(eq + 1);
        char name[256];
        int n = 0;
        if (strncasecmp(key, "MHFU_", 5) != 0)
            n = snprintf(name, sizeof name, "MHFU_");
        for (const char *k = key; *k && n < (int)sizeof name - 1; k++)
            name[n++] = (char)(*k >= 'a' && *k <= 'z' ? *k - 32 : *k);
        name[n] = 0;
        if (!strcmp(name, "MHFU_ISO")) {
            if (!iso_setting) {
                iso_setting = value[0] == '~' && getenv("HOME") ? malloc(strlen(getenv("HOME")) + strlen(value)) : NULL;
                if (iso_setting)
                    sprintf(iso_setting, "%s%s", getenv("HOME"), value + 1);
                else
                    iso_setting = strdup(value);
            }
        } else
            setenv(name, value, 0);
    }
    fclose(f);
}

/* What is played with unless a file or the environment says otherwise. Headless runs and replays leave them out, which is
 * what the tests' baselines are of; MHFU_DEFAULTS=0 leaves them out anywhere, =1 keeps them even there. */
static const struct { const char *name, *value; } setting_defaults[] = {
    {"MHFU_TICK_HZ", "60"}, {"MHFU_MOUSE_SENS", "0.1"},
    {"MHFU_SHADOWS", "1"}, {"MHFU_SUN_SHADING", "1"}, {"MHFU_RELIEF", "1"}, {"MHFU_PIXEL_LIGHTING", "1"},
    {"MHFU_POINT_LIGHTS", "1"}, {"MHFU_POINT_SHADOWS", "1"}, {"MHFU_AO", "1"}, {"MHFU_AO_STRENGTH", "1.0"},
    {"MHFU_GRASS", "0"}, {"MHFU_GRASS_DISTANCE", "6000"}, {"MHFU_GRASS_FULL", "800"}, {"MHFU_GRASS_DENSITY", "0.5"},
    {"MHFU_SHADOW_DISTANCE", "6000"}, {"MHFU_TESSELLATION", "0"}, {"MHFU_GPU_TL", "2"}, {"MHFU_SNOW_HAZE", "0"},
    {"MHFU_SWAY", "0.0"}, {"MHFU_MOTION_BLUR", "0.1"}, {"MHFU_DOF", "0"}, {"MHFU_CONTRAST", "1.0"},
    {"MHFU_HUD_SCALE", "0.75"}, {"MHFU_CAMERA_DISTANCE", "1.3"}, {"MHFU_TAA", "0"}, {"MHFU_FRAME_HISTORY", "60"},
};

static void apply_setting_defaults(void) {
    const char *e = getenv("MHFU_DEFAULTS");
    if (e ? atoi(e) == 0 : getenv("MHFU_HEADLESS") || getenv("MHFU_REPLAY"))
        return;
    for (size_t i = 0; i < sizeof setting_defaults / sizeof setting_defaults[0]; i++)
        setenv(setting_defaults[i].name, setting_defaults[i].value, 0);
}

static void settings(void) {
    load_settings("mhfu.ini");
    char path[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(path, sizeof path, "%s/mhfu-recomp/settings.ini", xdg);
    else if (home)
        snprintf(path, sizeof path, "%s/.config/mhfu-recomp/settings.ini", home);
    else
        path[0] = 0;
    if (path[0]) {
        load_settings(path);
        rt_settings_path = strdup(path);
    }
    apply_setting_defaults();
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
    settings();
    const char *iso = NULL, *eboot = "game/decrypted/EBOOT.elf";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--eboot") && i + 1 < argc)
            eboot = argv[++i];
        else if (!strcmp(argv[i], "--trace"))
            hle_trace = 1;
        else
            iso = argv[i];
    }
    if (!iso)
        iso = iso_setting;
    if (!iso) {
        fprintf(stderr, "usage: %s <game.iso> [--eboot path/to/EBOOT.elf] [--trace] (or iso = path in mhfu.ini)\n",
                argv[0]);
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
    pacing_init();
    window_init();
    hle_enabled = 1;

    /* The module's start: a thread at its entry, given the EBOOT's path as its argument */
    static const char path[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    Ctx proto = {0};
    proto.r[28] = gp;
    thread_create_main(&proto, entry, 0x20, 0x40000, sizeof path, path);
    scheduler_run();
}
