/* Savedata (the sceUtilitySavedata dialog's work): saves kept as plain files on the host, in
 * $MHFU_SAVEDIR or ~/.local/share/mhfu-recomp/SAVEDATA, one directory per save (game name + save name) - the data file,
 * and the PARAM.SFO details and icon the game provides. The game hands the utility its data unencrypted and gets it
 * back the same way, so nothing is encrypted here (saves from a PSP, which are, cannot be used as they are).
 *
 * SceUtilitySavedataParam: the dialog's common part (result at 0x1C), mode 0x30, game name 0x3C, save name 0x4C (with
 * "<>" for none), the list of save names 0x60, file name 0x64, data buffer 0x74, its size 0x78, the data's size 0x7C,
 * the SFO parameters 0x80 (title 0x80 bytes, save title 0x80, details 0x400, parental level), the icon / picture /
 * sound files 0x584 (four of: buffer, buffer size, size, unknown), ..., focus 0x5C8, the free / used space
 * structures for SIZES 0x5D0 0x5D4 0x5D8, key 0x5DC. */
#include "hle.h"

#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
    AUTOLOAD = 0, AUTOSAVE = 1, LOAD = 2, SAVE = 3, LISTLOAD = 4, LISTSAVE = 5, LISTDELETE = 6, DELETE = 7, SIZES = 8,
};

#define ERROR_LOAD_NO_DATA   0x80110307u
#define ERROR_LOAD_ACCESS    0x80110305u
#define ERROR_SAVE_ACCESS    0x80110385u
#define ERROR_SIZES_NO_DATA  0x801103C7u
#define ERROR_DELETE_NO_DATA 0x80110347u

static const char *save_root(void) {
    static char root[1024];
    if (!root[0]) {
        const char *e = getenv("MHFU_SAVEDIR");
        if (e) {
            snprintf(root, sizeof root, "%s", e);
        } else {
            const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
            if (data)
                snprintf(root, sizeof root, "%s/mhfu-recomp/SAVEDATA", data);
            else
                snprintf(root, sizeof root, "%s/.local/share/mhfu-recomp/SAVEDATA", home ? home : ".");
        }
    }
    return root;
}

static void make_dirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

/* A fixed-size string field from the parameters ("<>" is none) */
static void field(uint32_t addr, size_t n, char *out) {
    memcpy(out, PTR(addr), n);
    out[n] = 0;
    if (!strcmp(out, "<>"))
        out[0] = 0;
    for (char *s = out; *s; s++)
        if (*s == '/')
            *s = '_';
}

static void save_dir(uint32_t p, const char *save_name, char *out, size_t n) {
    char game[14];
    field(p + 0x3C, 13, game);
    snprintf(out, n, "%s/%s%s", save_root(), game, save_name);
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    int ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) ? -1 : (long)st.st_size;
}

static uint32_t do_load(uint32_t p, const char *save_name) {
    char dir[1024], file[14], path[1100];
    save_dir(p, save_name, dir, sizeof dir);
    field(p + 0x64, 13, file);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "rb");
    if (!f)
        return ERROR_LOAD_NO_DATA;
    uint32_t buf = LW(p + 0x74), cap = LW(p + 0x78);
    size_t n = fread(PTR(buf), 1, cap, f);
    fclose(f);
    SW(p + 0x7C, (uint32_t)n);
    /* the SFO details, if saved */
    snprintf(path, sizeof path, "%s/PARAM.SFO.bin", dir);
    f = fopen(path, "rb");
    if (f) {
        size_t got = fread(PTR(p + 0x80), 1, 0x504, f);
        (void)got;
        fclose(f);
    }
    HLE_LOG("savedata: loaded %s (%zu bytes)", path, n);
    return 0;
}

static uint32_t do_save(uint32_t p, const char *save_name) {
    char dir[1024], file[14], path[1100];
    save_dir(p, save_name, dir, sizeof dir);
    field(p + 0x64, 13, file);
    make_dirs(dir);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    if (!write_file(path, PTR(LW(p + 0x74)), LW(p + 0x7C)))
        return ERROR_SAVE_ACCESS;
    snprintf(path, sizeof path, "%s/PARAM.SFO.bin", dir);
    write_file(path, PTR(p + 0x80), 0x504);
    static const char *names[4] = {"ICON0.PNG", "ICON1.PMF", "PIC1.PNG", "SND0.AT3"};
    for (int i = 0; i < 4; i++) {
        uint32_t fd = p + 0x584 + 16 * (uint32_t)i, buf = LW(fd), size = LW(fd + 8);
        if (buf && size) {
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
            write_file(path, PTR(buf), size);
        }
    }
    HLE_WARN("savedata: saved %s/%s (%u bytes)", dir, file, LW(p + 0x7C));
    return 0;
}

static uint32_t do_delete(uint32_t p, const char *save_name) {
    char dir[1024], path[1300];
    save_dir(p, save_name, dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d)
        return ERROR_DELETE_NO_DATA;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            remove(path);
        }
    closedir(d);
    rmdir(dir);
    return 0;
}

static void kb_string(uint32_t addr, uint32_t kb) {
    char s[8];
    snprintf(s, sizeof s, "%u KB", kb);
    memcpy(PTR(addr), s, 8);
}

/* SIZES: free space (cluster size, free clusters, free KB, as text), the named save's use, and what saving this one
 * would need */
static uint32_t do_sizes(uint32_t p) {
    uint32_t ms_free = LW(p + 0x5D0), ms_data = LW(p + 0x5D4), used = LW(p + 0x5D8);
    if (ms_free) {
        SW(ms_free, 0x8000);
        SW(ms_free + 4, 0x8000);            /* 1 GB free */
        SW(ms_free + 8, 0x100000);
        kb_string(ms_free + 12, 0x100000);
    }
    uint32_t result = 0;
    if (ms_data) {
        char save[21], dir[1024], file[14], path[1100];
        field(ms_data + 16, 20, save);
        save_dir(p, save, dir, sizeof dir);
        field(p + 0x64, 13, file);
        snprintf(path, sizeof path, "%s/%s", dir, file);
        long sz = file_size(path);
        uint32_t kb = sz < 0 ? 0 : (uint32_t)((sz + 1023) / 1024);
        SW(ms_data + 36, (kb + 31) / 32);
        SW(ms_data + 40, kb);
        kb_string(ms_data + 44, kb);
        SW(ms_data + 52, (kb + 31) / 32 * 32);
        kb_string(ms_data + 56, (kb + 31) / 32 * 32);
        if (sz < 0)
            result = ERROR_SIZES_NO_DATA;
    }
    if (used) {
        uint32_t kb = (LW(p + 0x78) + 1023) / 1024 + 64;        /* the data, and room for the icon and details */
        SW(used, (kb + 31) / 32);
        SW(used + 4, kb);
        kb_string(used + 8, kb);
        SW(used + 16, (kb + 31) / 32 * 32);
        kb_string(used + 20, (kb + 31) / 32 * 32);
    }
    return result;
}

/* The first save name of the list (0x60: names of 20 bytes, ending with an empty one) with data, for the list modes,
 * where the user would choose; else the first */
static void list_choice(uint32_t p, int existing, char *out) {
    uint32_t list = LW(p + 0x60);
    out[0] = 0;
    if (!list) {
        field(p + 0x4C, 20, out);
        return;
    }
    for (int i = 0; i < 100; i++) {
        char name[21], dir[1024];
        field(list + 20 * (uint32_t)i, 20, name);
        if (!name[0] && i > 0)
            break;
        save_dir(p, name, dir, sizeof dir);
        struct stat st;
        int exists = !stat(dir, &st);
        if (i == 0)
            strcpy(out, name);
        if (exists == existing) {
            strcpy(out, name);
            return;
        }
    }
}

void savedata_run(uint32_t p) {
    uint32_t mode = LW(p + 0x30), result;
    char save[21];
    field(p + 0x4C, 20, save);
    switch (mode) {
    case AUTOLOAD: case LOAD: result = do_load(p, save); break;
    case AUTOSAVE: case SAVE: result = do_save(p, save); break;
    case LISTLOAD: list_choice(p, 1, save); result = do_load(p, save); break;
    case LISTSAVE: list_choice(p, 0, save); result = do_save(p, save); break;
    case DELETE: result = do_delete(p, save); break;
    case LISTDELETE: list_choice(p, 1, save); result = do_delete(p, save); break;
    case SIZES: result = do_sizes(p); break;
    default:
        HLE_WARN("savedata mode %u not implemented", mode);
        result = 0;
        break;
    }
    if (mode == LISTLOAD || mode == LISTSAVE || mode == LISTDELETE)
        memcpy(PTR(p + 0x4C), save, 20);
    SW(p + 0x1C, result);
    HLE_LOG("savedata mode %u %s: %08X", mode, save, result);
}
