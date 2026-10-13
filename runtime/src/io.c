/* File I/O: the disc (disc0:, umd0:, umd1:) read from the ISO image through a small ISO 9660 reader - files by path,
 * and raw sectors by the kernel's "sce_lbn0x<sector>_size0x<size>" names; stdout / stderr to the log. */
#include "hle.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SECTOR 2048

static FILE *iso;

typedef struct { uint32_t lba, size; int is_dir; } Entry;

static int read_sectors(uint32_t lba, uint32_t offset, void *buf, uint32_t size) {
    if (fseeko(iso, (off_t)lba * SECTOR + offset, SEEK_SET))
        return -1;
    return (int)fread(buf, 1, size, iso);
}

static Entry root;

void io_init(const char *iso_path) {
    iso = fopen(iso_path, "rb");
    if (!iso) {
        perror(iso_path);
        exit(1);
    }
    uint8_t pvd[SECTOR];
    if (read_sectors(16, 0, pvd, SECTOR) != SECTOR || memcmp(pvd + 1, "CD001", 5)) {
        fprintf(stderr, "%s: not an ISO 9660 image\n", iso_path);
        exit(1);
    }
    memcpy(&root.lba, pvd + 156 + 2, 4);
    memcpy(&root.size, pvd + 156 + 10, 4);
    root.is_dir = 1;
}

/* Look up one name in a directory */
static int find_in_dir(const Entry *dir, const char *name, size_t len, Entry *out) {
    uint8_t *buf = malloc(dir->size);
    if (read_sectors(dir->lba, 0, buf, dir->size) != (int)dir->size) {
        free(buf);
        return 0;
    }
    int found = 0;
    for (uint32_t off = 0; off < dir->size;) {
        uint8_t reclen = buf[off];
        if (!reclen) {                              /* records do not cross sectors: on to the next */
            off = (off / SECTOR + 1) * SECTOR;
            continue;
        }
        uint8_t namelen = buf[off + 32];
        const char *n = (const char *)buf + off + 33;
        size_t nl = namelen;
        if (nl >= 2 && n[nl - 2] == ';')
            nl -= 2;
        if (nl == len && !strncasecmp(n, name, len)) {
            memcpy(&out->lba, buf + off + 2, 4);
            memcpy(&out->size, buf + off + 10, 4);
            out->is_dir = (buf[off + 25] & 2) != 0;
            found = 1;
            break;
        }
        off += reclen;
    }
    free(buf);
    return found;
}

/* A path on the disc device, after its "disc0:" prefix */
static int lookup_path(const char *path, Entry *out) {
    unsigned long lbn, size;
    if (sscanf(path, "/sce_lbn0x%lx_size0x%lx", &lbn, &size) == 2 || sscanf(path, "sce_lbn0x%lx_size0x%lx", &lbn, &size) == 2) {
        out->lba = (uint32_t)lbn;
        out->size = (uint32_t)size;
        out->is_dir = 0;
        return 1;
    }
    Entry cur = root;
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        const char *end = strchr(path, '/');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        if (!cur.is_dir || !find_in_dir(&cur, path, len, &cur))
            return 0;
        path += len;
    }
    *out = cur;
    return 1;
}

static const char *disc_path(const char *path) {
    static const char *devices[] = {"disc0:", "umd0:", "umd1:", "umd:"};
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++)
        if (!strncasecmp(path, devices[i], strlen(devices[i])))
            return path + strlen(devices[i]);
    return NULL;
}

/* -- file descriptors --------------------------------------------------------------------------------------------- */

#define MAX_FDS 64
static struct { int open; Entry e; uint64_t pos; } fds[MAX_FDS];

void hle_sceIoOpen(Ctx *c) {
    const char *path = hle_string(c, ARG0);
    const char *p = disc_path(path);
    Entry e;
    if (!p || !lookup_path(p, &e) || e.is_dir) {
        HLE_WARN("sceIoOpen(%s): not found", path);
        RET(SCE_ERROR_ERRNO_FILE_NOT_FOUND);
        return;
    }
    for (int fd = 3; fd < MAX_FDS; fd++)
        if (!fds[fd].open) {
            fds[fd].open = 1;
            fds[fd].e = e;
            fds[fd].pos = 0;
            HLE_LOG("%d=sceIoOpen(%s)", fd, path);
            RET(fd);
            return;
        }
    RET(0x80010018u);       /* too many open files */
}

void hle_sceIoClose(Ctx *c) {
    uint32_t fd = ARG0;
    if (fd >= MAX_FDS || !fds[fd].open) {
        RET(SCE_ERROR_ERRNO_BAD_FD);
        return;
    }
    fds[fd].open = 0;
    RET(0);
}

void hle_sceIoRead(Ctx *c) {
    uint32_t fd = ARG0, buf = ARG1, size = ARG2;
    if (fd >= MAX_FDS || !fds[fd].open) {
        RET(SCE_ERROR_ERRNO_BAD_FD);
        return;
    }
    uint64_t left = fds[fd].e.size > fds[fd].pos ? fds[fd].e.size - fds[fd].pos : 0;
    if (size > left)
        size = (uint32_t)left;
    if (fseeko(iso, (off_t)fds[fd].e.lba * SECTOR + (off_t)fds[fd].pos, SEEK_SET)) {
        RET(0x80010005u);
        return;
    }
    size_t got = fread(PTR(buf), 1, size, iso);
    fds[fd].pos += got;
    RET(got);
}

void hle_sceIoWrite(Ctx *c) {
    uint32_t fd = ARG0;
    if (fd == 1 || fd == 2) {
        fprintf(stderr, "game: %.*s", (int)ARG2, (const char *)PTR(ARG1));
        RET(ARG2);
        return;
    }
    RET(0x8001001Eu);       /* read-only file system */
}

/* (fd, offset: a2:a3, whence: t0) -> the new position, 64-bit */
void hle_sceIoLseek(Ctx *c) {
    uint32_t fd = ARG0;
    int64_t off = (int64_t)(ARG2 | ((uint64_t)ARG3 << 32));
    if (fd >= MAX_FDS || !fds[fd].open) {
        RET64((int64_t)(int32_t)SCE_ERROR_ERRNO_BAD_FD);
        return;
    }
    int64_t base = ARG4 == 0 ? 0 : ARG4 == 1 ? (int64_t)fds[fd].pos : (int64_t)fds[fd].e.size;
    int64_t pos = base + off;
    if (pos < 0)
        pos = 0;
    fds[fd].pos = (uint64_t)pos;
    RET64(pos);
}

/* SceIoStat: mode, attr, size (64-bit, at 8), three ScePspDateTime (16 bytes each), private[6] (sector first) */
void hle_sceIoGetstat(Ctx *c) {
    const char *path = hle_string(c, ARG0);
    const char *p = disc_path(path);
    Entry e;
    if (!p || !lookup_path(p, &e)) {
        RET(SCE_ERROR_ERRNO_FILE_NOT_FOUND);
        return;
    }
    uint8_t *st = PTR(ARG1);
    memset(st, 0, 88);
    uint32_t mode = e.is_dir ? 0x116D : 0x216D, attr = e.is_dir ? 0x10 : 0x20;
    memcpy(st, &mode, 4);
    memcpy(st + 4, &attr, 4);
    uint64_t size = e.size;
    memcpy(st + 8, &size, 8);
    memcpy(st + 64, &e.lba, 4);
    RET(0);
}

void hle_sceIoDevctl(Ctx *c) {
    uint32_t cmd = ARG1;
    HLE_LOG("sceIoDevctl(%s, %08X)", hle_string(c, ARG0), cmd);
    RET(0);
}

void hle_sceIoDopen(Ctx *c) { RET(SCE_ERROR_ERRNO_FILE_NOT_FOUND); }
void hle_sceIoDclose(Ctx *c) { RET(0); }
void hle_sceIoRename(Ctx *c) { RET(0x8001001Eu); }
