/* The settings changed while the game runs (settings.c): the settings screen's list (overlay.c), and how what reads a
 * setting where it is used knows to read it again. */
#pragma once
#include <stddef.h>

/* Counts the changes. A setting read where it is used is read again after one:
 *     static unsigned seen; static int on;
 *     if (SETTING_STALE(seen)) on = ...getenv("MHFU_...")...;
 * (from 1: a static seen, 0, is stale the first time). Changed on the game's thread with the renderer idle. */
extern unsigned rt_settings_generation;
#define SETTING_STALE(seen) ((seen) != rt_settings_generation ? ((seen) = rt_settings_generation, 1) : 0)

enum { S_HEADING, S_TOGGLE, S_NUMBER, S_CHOICE };
enum { S_LIVE, S_RESTART };

typedef struct {
    const char *key;                    /* as in settings.ini ("shadows": MHFU_SHADOWS in the environment) */
    const char *label;
    int kind;
    const char *unset;                  /* its value where nothing sets it */
    float lo, hi, step;                 /* S_NUMBER */
    const char *choices;                /* S_CHOICE: "value=LABEL|value=LABEL..." */
    int when;                           /* S_LIVE: at once; S_RESTART: when the game is next started */
} Setting;

int settings_count(void);
const Setting *settings_at(int i);
/* Its value as shown ("ON", "1.25", a choice's label); 1 if it waits for a restart (changed, and S_RESTART) */
int settings_shown(int i, char *out, size_t size);
void settings_step(int i, int dir);     /* the next value (dir 1) or the one before (-1); a toggle: the other */
void settings_set(const char *key, const char *value);  /* (any MHFU_ setting, by its settings.ini name) */
void settings_save(void);               /* what was changed, into settings.ini (its other lines left as they are) */
void settings_undo(void);               /* back to how they were when settings_keep was last called */
void settings_keep(void);
void settings_frame(unsigned long frame);   /* present.c, a game frame: MHFU_TEST_SETTING's changes */
