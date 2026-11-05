/* The game's objects, read: the object manager at *0x08A62D7C has 15 lists, heads at +0x80028, next at +0x1C4 (EBOOT
 * 088662B8, 088666B4). Reads only. A list being empty or the manager not there yet is normal (the title). What is past
 * the base's fields is only read for hunters and monsters: other classes are smaller. */
#include "objects.h"
#include "hle.h"

#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t vtable; const char *name, *conf; } ClassName;
static const ClassName class_names[] = {
#include "class_table.h"
};

static int cmp_class(const void *key, const void *row) {
    uint32_t k = *(const uint32_t *)key, v = ((const ClassName *)row)->vtable;
    return k < v ? -1 : k > v;
}

static int in_ram(uint32_t a) {
    return a >= 0x08800000u && a < 0x0A000000u;
}

/* The game's list of monster names (in the EBOOT's data, as classes.toml says): by the type byte. Inferred: the list isn't
 * shown to be read by that byte; the name is taken from the copy of the game, none of it is in the repository */
#define NAMES_AT 0x089CE39Eu
static const char *species_name(int type) {
    static const char *names[256];
    static int n = -1;
    if (n < 0) {
        n = 0;
        if (!memcmp(MEM(NAMES_AT), "NO DATA", 8)) {
            uint32_t a = NAMES_AT;
            while (n < 256) {
                const char *s = (const char *)MEM(a);
                size_t len = strnlen(s, 40);
                if (len == 40)
                    break;
                names[n++] = s;
                a += (uint32_t)len + 1;
            }
        } else
            n = -1;                     /* (not loaded yet: asked again) */
    }
    return n > 0 && type >= 0 && type < n && names[type][0] ? names[type] : NULL;
}

const char *object_kind_name(int kind) {
    return kind == 0 ? "hunter" : kind == 1 ? "monster" : kind == 2 ? "NPC" : kind == 3 ? "base" : "?";
}

int object_read(uint32_t addr, int list, GameObject *o) {
    if (!in_ram(addr) || (addr & 3) || !in_ram(addr + 0x420))
        return 0;
    uint32_t vt = LW(addr);
    if (vt < 0x08804000u || vt >= 0x0A000000u || (vt & 3))
        return 0;
    memset(o, 0, sizeof *o);
    o->addr = addr;
    o->vtable = vt;
    o->list = list;
    o->flags = LW(addr + 4);
    const ClassName *c = bsearch(&vt, class_names, sizeof class_names / sizeof class_names[0], sizeof class_names[0], cmp_class);
    o->cls = c ? c->name : NULL;
    o->conf = c ? c->conf : NULL;
    o->kind = LBU(addr + 0x1E9);
    o->type = LBU(addr + 0x1E8);
    o->life = LBU(addr + 0x1D4);
    o->group = LBU(addr + 0x298);
    o->state = LBU(addr + 0x299);
    o->area = LHU(addr + 0x29A);
    o->motion = LHU(addr + 0x324);
    memcpy(o->place, MEM(addr + 0x200), sizeof o->place);
    memcpy(&o->ticks, MEM(addr + 0x3F4), sizeof o->ticks);
    if ((list == 2 || list == 3) && o->kind <= 1) {
        o->has_health = 1;
        o->health = (int16_t)LHU(addr + 0x2E4);
        o->health_max = (int16_t)LHU(addr + 0x41E);
    }
    if (o->kind == 1)
        o->species = species_name(o->type);
    return 1;
}

int objects_read(GameObject *out, int max) {
    uint32_t mgr = LW(0x08A62D7Cu);
    if (!in_ram(mgr) || !in_ram(mgr + 0x80064))
        return 0;
    int n = 0;
    for (int list = 0; list < 15; list++) {
        uint32_t a = LW(mgr + 0x80028u + 4 * (uint32_t)list);
        for (int steps = 0; a && steps < 4096 && n < max; steps++) {
            if (!object_read(a, list, &out[n]))
                break;
            a = LW(a + 0x1C4);
            n++;
        }
    }
    return n;
}

/* state_ticks (+0x3F4): a float as read, but not every class's is one: the raw word then, not a nonsense number */
const char *object_ticks_text(const GameObject *o) {
    static char t[32];
    uint32_t word;
    memcpy(&word, &o->ticks, 4);
    if (o->ticks == o->ticks && o->ticks > -1e7f && o->ticks < 1e7f)
        snprintf(t, sizeof t, "%.1f", (double)o->ticks);
    else
        snprintf(t, sizeof t, "word %08X", word);
    return t;
}

void object_print(FILE *f, const GameObject *o) {
    fprintf(f, "%08X vt %08X list %d class %s", o->addr, o->vtable, o->list, o->cls ? o->cls : "(unnamed)");
    if (o->cls)
        fprintf(f, " (%s)", o->conf);
    fprintf(f, " kind %d (%s) type 0x%02X", o->kind, object_kind_name(o->kind), o->type);
    if (o->species)
        fprintf(f, " species %s (inferred)", o->species);
    fprintf(f, " place %.1f %.1f %.1f state %d/%d motion 0x%X ticks %s life %d flags %08X", (double)o->place[0],
            (double)o->place[1], (double)o->place[2], o->group, o->state, o->motion, object_ticks_text(o), o->life, o->flags);
    if (o->has_health)
        fprintf(f, " health %d/%d", o->health, o->health_max);
    fprintf(f, " area %d\n", o->area);
}

void game_where(int *stage, int *map, int *quest) {
    uint32_t game = LW(0x089CC558u);
    if (!in_ram(game) || !in_ram(game + 0x6AF14)) {
        *stage = *map = *quest = -1;
        return;
    }
    *stage = LHU(game + 0x6AF0E);
    *map = LBU(game + 0x6AF10);
    *quest = LHU(game + 0x6AF12);
}
