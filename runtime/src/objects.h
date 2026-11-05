/* The game's objects, read (objects.c): the debug tools' view. Names are mechanical where unconfirmed: a class's name
 * comes with its confidence from classes.toml; a species is the game's name list read by the type byte, inferred. */
#pragma once
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t addr, vtable, flags;
    int list;                           /* its list in the object manager: 2 players, 3 monsters and other creatures */
    const char *cls, *conf;             /* class name (NULL: unnamed) and how sure; species (NULL: none) */
    const char *species;
    int kind, type;                     /* bytes +0x1E9 (0 hunter, 1 monster, 2 NPC, 3 the base's default), +0x1E8 */
    int group, state, motion, life, area;
    int has_health, health, health_max;  /* (kinds 0 and 1) */
    float place[3], ticks;
} GameObject;

int objects_read(GameObject *out, int max);       /* every object in the manager's lists; how many */
int object_read(uint32_t addr, int list, GameObject *o);  /* one, 0 if it isn't one now (its vtable checked by the caller) */
void object_print(FILE *f, const GameObject *o);
const char *object_kind_name(int kind);
const char *object_ticks_text(const GameObject *o);
void game_where(int *stage, int *map, int *quest);        /* -1 if the game has no state yet */
