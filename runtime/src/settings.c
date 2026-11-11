/* The settings, changed while the game runs. A setting is an MHFU_ environment variable (main.c: the environment, then
 * mhfu.ini, then settings.ini, then the defaults), read where it is used; changing one here sets the variable and
 * counts the change (rt_settings_generation), and what reads it reads it again (SETTING_STALE, settings.h) - at once
 * for most, as the settings screen shows them (overlay.c: the game paused, the frame under it drawn again with them).
 * A few are read once as the game starts (S_RESTART): those are saved and said to wait. What takes more than reading
 * again is told here: the internal resolution (render.c), the window's full screen (window.c), the mouse's
 * sensitivity (camera.c: a recording keeps its own).
 *
 * settings_save writes what was changed into settings.ini: the key's line replaced where there is one (a commented
 * one taken over), a line added otherwise; the rest of the file is left as it is. The environment still wins when the
 * game is next started: a setting given there is as given, whatever the file says.
 *
 * MHFU_TEST_SETTING="frame,key=value[;frame,key=value...]": changed from the game's frame (for testing: a picture
 * after a change should be the picture of a run started with it); "frame,save=1" saves as closing the screen does. */
#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

unsigned rt_settings_generation = 1;
extern const char *rt_settings_path;

#define RESOLUTIONS "=AS THE WINDOW|960x544=960 X 544|1280x720=1280 X 720|1920x1080=1920 X 1080|2560x1440=2560 X 1440|" \
                    "3840x2160=3840 X 2160|5120x2880=5120 X 2880"

static const Setting list[] = {
    {NULL, "DISPLAY", S_HEADING},
    {"resolution", "INTERNAL RESOLUTION", S_CHOICE, "", 0, 0, 0, RESOLUTIONS, S_LIVE},
    {"fullscreen", "FULL SCREEN", S_TOGGLE, "0", 0, 0, 0, NULL, S_LIVE},
    {"fill", "FILL A WIDE SCREEN", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"fps", "FRAMES A SECOND SHOWN", S_CHOICE, "0", 0, 0, 0, "0=AS THE GAME|60=60|75=75|90=90|120=120|144=144|165=165|240=240",
     S_RESTART},
    {"tick_hz", "GAME FRAMES A SECOND", S_CHOICE, "30", 0, 0, 0, "30=30 (AS THE PSP)|60=60", S_RESTART},
    {"taa", "ANTI-ALIASING (TEMPORAL)", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"mipmaps", "MIPMAPS (FAR TEXTURES STEADY)", S_TOGGLE, "1", 0, 0, 0, NULL, S_RESTART},
    {"tessellation", "ROUNDER CHARACTERS (TESSELLATION)", S_TOGGLE, "1", 0, 0, 0, NULL, S_RESTART},
    {"gpu_tl", "VERTICES ON THE GPU", S_CHOICE, "2", 0, 0, 0, "0=NONE|1=CHARACTERS|2=ALL", S_RESTART},

    {NULL, "LIGHT AND SHADOW", S_HEADING},
    {"shadows", "SUN SHADOWS", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"shadow_distance", "SHADOW DISTANCE", S_NUMBER, "3000", 1500, 12000, 500, NULL, S_LIVE},
    {"shadow_resolution", "SHADOW MAP SIZE", S_CHOICE, "4096", 0, 0, 0, "1024=1024|2048=2048|4096=4096|8192=8192", S_RESTART},
    {"sun_shading", "SUN SHADING OF THE SCENERY", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"relief", "RELIEF FROM TEXTURES", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"pixel_lighting", "LIGHTING BY THE PIXEL", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"point_lights", "LIGHT FROM FIRES", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"point_shadows", "SHADOWS FROM FIRES", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"ao", "AMBIENT OCCLUSION", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"ao_strength", "AMBIENT OCCLUSION STRENGTH", S_NUMBER, "2.0", 0, 2, 0.1f, NULL, S_LIVE},
    {"blob_shadows", "ROUND SHADOWS UNDER FEET KEPT", S_TOGGLE, "0", 0, 0, 0, NULL, S_LIVE},
    {"painted_shadows", "PAINTED BUILDING SHADOWS KEPT", S_TOGGLE, "0", 0, 0, 0, NULL, S_LIVE},
    {"backdrop_shadows", "FAR SCENERY CASTS SHADOWS", S_TOGGLE, "0", 0, 0, 0, NULL, S_LIVE},

    {NULL, "THE WORLD", S_HEADING},
    {"grass", "GROUND GRASS", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"grass_density", "GRASS DENSITY", S_NUMBER, "1.0", 0.25f, 2, 0.25f, NULL, S_RESTART},
    {"grass_distance", "GRASS DISTANCE", S_NUMBER, "3000", 1000, 10000, 500, NULL, S_RESTART},
    {"sway", "PLANTS SWAY IN THE WIND", S_NUMBER, "1.0", 0, 2, 0.1f, NULL, S_LIVE},
    {"water", "WATER (REFLECTING, RIPPLED)", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"snow_haze", "SNOW HAZE", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"sun_flare", "ORIGINAL SUN FLARE KEPT", S_TOGGLE, "0", 0, 0, 0, NULL, S_LIVE},
    {"wounds", "WOUNDS ON MONSTERS", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"wound_size", "WOUND SIZE", S_NUMBER, "1.0", 0.5f, 4, 0.25f, NULL, S_LIVE},
    {"blood_opacity", "BLOOD ON MONSTERS: HOW SOLID", S_NUMBER, "1.0", 0, 1, 0.05f, NULL, S_LIVE},
    {"blood_shine", "BLOOD ON MONSTERS: HOW IT SHINES", S_NUMBER, "0.6", 0, 2, 0.1f, NULL, S_LIVE},

    {NULL, "THE PICTURE", S_HEADING},
    {"post", "POST-PROCESSING", S_TOGGLE, "1", 0, 0, 0, NULL, S_LIVE},
    {"bloom", "BLOOM", S_NUMBER, "3.0", 0, 6, 0.5f, NULL, S_LIVE},
    {"motion_blur", "MOTION BLUR", S_NUMBER, "0.5", 0, 1, 0.05f, NULL, S_LIVE},
    {"dof", "DEPTH OF FIELD", S_NUMBER, "0.0", 0, 2, 0.25f, NULL, S_LIVE},
    {"sharpen", "SHARPENING", S_NUMBER, "0.4", 0, 1, 0.1f, NULL, S_LIVE},
    {"contrast", "CONTRAST", S_NUMBER, "1.05", 0.8f, 1.3f, 0.01f, NULL, S_LIVE},
    {"saturation", "SATURATION", S_NUMBER, "1.08", 0.5f, 1.5f, 0.02f, NULL, S_LIVE},
    {"warmth", "WARMTH", S_NUMBER, "0.0", -1, 1, 0.05f, NULL, S_LIVE},
    {"vignette", "VIGNETTE", S_NUMBER, "0.2", 0, 1, 0.05f, NULL, S_LIVE},
    {"haze", "HAZE IN THE AIR", S_NUMBER, "0.0", 0, 4, 0.25f, NULL, S_LIVE},
    {"shafts", "LIGHT SHAFTS", S_NUMBER, "0.0", 0, 2, 0.25f, NULL, S_LIVE},
    {"weapon_blood", "BLOOD THROWN OFF THE WEAPON", S_NUMBER, "1.0", 0, 4, 0.25f, NULL, S_LIVE},
    {"weapon_blood_size", "SIZE OF THE WEAPON'S DROPS OF BLOOD", S_NUMBER, "1.0", 0.25f, 4, 0.25f, NULL, S_LIVE},

    {NULL, "VIEW", S_HEADING},
    {"hud_scale", "HUD SIZE", S_NUMBER, "1.0", 0.3f, 1.5f, 0.05f, NULL, S_LIVE},
    {"camera_distance", "CAMERA DISTANCE", S_NUMBER, "1.0", 0.5f, 3, 0.05f, NULL, S_LIVE},
    {"mouse_sens", "MOUSE SENSITIVITY (DEGREES A COUNT)", S_NUMBER, "0.15", 0.02f, 0.5f, 0.01f, NULL, S_LIVE},
    {"camera_assist", "THE GAME TURNS THE CAMERA ITSELF", S_TOGGLE, "0", 0, 0, 0, NULL, S_RESTART},
};
#define COUNT ((int)(sizeof list / sizeof list[0]))

static char started[COUNT][32], kept[COUNT][32];    /* values as the game started; as when last kept (settings_keep) */
static int changed[COUNT], loaded;

int settings_count(void) { return COUNT; }
const Setting *settings_at(int i) { return &list[i]; }

static void env_name(const char *key, char *out, size_t size) {
    size_t n = (size_t)snprintf(out, size, "MHFU_");
    for (; *key && n < size - 1; key++)
        out[n++] = (char)(*key >= 'a' && *key <= 'z' ? *key - 32 : *key);
    out[n] = 0;
}

static const char *value_of(int i) {
    char name[64];
    env_name(list[i].key, name, sizeof name);
    const char *v = getenv(name);
    return v ? v : list[i].unset;
}

static void load(void) {
    if (loaded)
        return;
    loaded = 1;
    for (int i = 0; i < COUNT; i++)
        if (list[i].key) {
            snprintf(started[i], sizeof started[i], "%s", value_of(i));
            snprintf(kept[i], sizeof kept[i], "%s", value_of(i));
        }
}

/* A choice's value and label, the n'th (0 if there is none) */
static int choice(const char *choices, int n, char *value, size_t vsize, char *label, size_t lsize) {
    const char *c = choices;
    for (int k = 0; c && *c; k++) {
        const char *end = strchr(c, '|'), *eq = strchr(c, '=');
        if (!end)
            end = c + strlen(c);
        if (k == n) {
            snprintf(value, vsize, "%.*s", (int)(eq - c), c);
            snprintf(label, lsize, "%.*s", (int)(end - eq - 1), eq + 1);
            return 1;
        }
        c = *end ? end + 1 : end;
    }
    return 0;
}

static int on(const char *v) { return atof(v) != 0; }
int render_grass_made(void);

static int decimals(float step) { return step >= 1 ? 0 : step >= 0.1f ? 1 : 2; }

int settings_shown(int i, char *out, size_t size) {
    load();
    const Setting *s = &list[i];
    out[0] = 0;
    if (s->kind == S_HEADING)
        return 0;
    const char *v = value_of(i);
    if (s->kind == S_TOGGLE)
        snprintf(out, size, "%s", on(v) ? "ON" : "OFF");
    else if (s->kind == S_NUMBER)
        snprintf(out, size, "%.*f", decimals(s->step), atof(v));
    else {
        char value[32], label[48];
        snprintf(out, size, "%s", v);       /* (not one of the choices: as it is) */
        for (int k = 0; choice(s->choices, k, value, sizeof value, label, sizeof label); k++)
            if (!strcasecmp(value, v) || (value[0] && v[0] && strspn(v, "0123456789.") == strlen(v) && atof(value) == atof(v)))
                snprintf(out, size, "%s", label);
    }
    /* (the grass can be turned on only where it was on as the game started: render_grass.c) */
    if (!strcmp(s->key, "grass") && on(v) && !render_grass_made())
        return !on(started[i]);
    /* (no resolution set, but a scale: that is what it is) */
    if (!strcmp(s->key, "resolution") && !v[0] && getenv("MHFU_SCALE") && atof(getenv("MHFU_SCALE")) >= 1)
        snprintf(out, size, "%s TIMES THE PSP", getenv("MHFU_SCALE"));
    return s->when == S_RESTART && strcmp(v, started[i]) != 0;
}

void window_set_fullscreen(int on);
void render_resolution_changed(void);
void camera_set_sens(float s);

static void set_at(int i, const char *value) {
    load();
    char name[64];
    env_name(list[i].key, name, sizeof name);
    if (value[0])
        setenv(name, value, 1);
    else
        unsetenv(name);
    changed[i] = 1;
    rt_settings_generation++;
    if (!strcmp(list[i].key, "fullscreen"))
        window_set_fullscreen(on(value));
    else if (!strcmp(list[i].key, "resolution") || !strcmp(list[i].key, "fill"))
        render_resolution_changed();
    else if (!strcmp(list[i].key, "mouse_sens"))
        camera_set_sens((float)atof(value));
}

void settings_set(const char *key, const char *value) {
    for (int i = 0; i < COUNT; i++)
        if (list[i].key && !strcasecmp(list[i].key, key)) {
            set_at(i, value);
            return;
        }
    char name[64];                      /* (not one of the screen's: set all the same) */
    env_name(key, name, sizeof name);
    setenv(name, value, 1);
    rt_settings_generation++;
}

void settings_step(int i, int dir) {
    load();
    const Setting *s = &list[i];
    const char *v = value_of(i);
    char next[32];
    if (s->kind == S_TOGGLE)
        snprintf(next, sizeof next, "%d", !on(v));
    else if (s->kind == S_NUMBER) {
        /* (in steps from the least, so that a value from the file between two steps lands on one) */
        float n = roundf(((float)atof(v) - s->lo) / s->step) + (float)dir;
        float x = s->lo + n * s->step;
        x = x < s->lo ? s->lo : x > s->hi ? s->hi : x;
        snprintf(next, sizeof next, "%.*f", decimals(s->step), x);
    } else if (s->kind == S_CHOICE) {
        char value[32], label[48];
        int n = 0, at = -1;
        for (; choice(s->choices, n, value, sizeof value, label, sizeof label); n++)
            if (!strcasecmp(value, v) || (value[0] && v[0] && strspn(v, "0123456789.") == strlen(v) && atof(value) == atof(v)))
                at = n;
        at = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
        choice(s->choices, at, next, sizeof next, label, sizeof label);
    } else
        return;
    set_at(i, next);
}

void settings_keep(void) {
    load();
    for (int i = 0; i < COUNT; i++)
        if (list[i].key)
            snprintf(kept[i], sizeof kept[i], "%s", value_of(i));
}

void settings_undo(void) {
    load();
    for (int i = 0; i < COUNT; i++)
        if (list[i].key && strcmp(kept[i], value_of(i)))
            set_at(i, kept[i]);
}

/* The key of a settings.ini line ("key = value", "#key = value"): into out; 1 if it is commented */
static int line_key(const char *line, char *out, size_t size, int *commented) {
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    *commented = *p == '#';
    if (*commented)
        p++;
    while (*p == ' ' || *p == '\t')
        p++;
    const char *eq = strchr(p, '=');
    if (!eq)
        return 0;
    size_t n = 0;
    for (; p < eq && *p != ' ' && *p != '\t' && n < size - 1; p++)
        out[n++] = *p;
    out[n] = 0;
    while (p < eq && (*p == ' ' || *p == '\t'))
        p++;
    if (p != eq || !n)
        return 0;
    if (!strncasecmp(out, "MHFU_", 5))
        memmove(out, out + 5, strlen(out + 5) + 1);
    return 1;
}

void settings_save(void) {
    load();
    int any = 0;
    for (int i = 0; i < COUNT; i++)
        any |= changed[i];
    if (!any || !rt_settings_path)
        return;
    /* the file's lines; each changed key's line: an uncommented one, else its first commented one */
    char **lines = NULL;
    int n = 0, cap = 0;
    FILE *f = fopen(rt_settings_path, "r");
    char buf[4096];
    while (f && fgets(buf, sizeof buf, f)) {
        if (n == cap)
            lines = realloc(lines, sizeof *lines * (size_t)(cap = cap ? cap * 2 : 64));
        buf[strcspn(buf, "\r\n")] = 0;
        lines[n++] = strdup(buf);
    }
    if (f)
        fclose(f);
    for (int i = 0; i < COUNT; i++) {
        if (!changed[i])
            continue;
        int at = -1, at_commented = -1;
        for (int l = 0; l < n; l++) {
            char key[64];
            int commented;
            if (!line_key(lines[l], key, sizeof key, &commented) || strcasecmp(key, list[i].key))
                continue;
            if (!commented)
                at = l;
            else if (at_commented < 0)
                at_commented = l;
        }
        const char *v = value_of(i);
        char line[128];
        snprintf(line, sizeof line, "%s%s = %s", v[0] ? "" : "#", list[i].key, v[0] ? v : started[i]);
        if (at < 0 && !v[0])
            continue;                   /* (unset, and not in the file: nothing to say) */
        if (at < 0)
            at = at_commented;
        if (at >= 0) {
            free(lines[at]);
            lines[at] = strdup(line);
        } else {
            if (n == cap)
                lines = realloc(lines, sizeof *lines * (size_t)(cap = cap ? cap * 2 : 64));
            lines[n++] = strdup(line);
        }
        changed[i] = 0;
    }
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.new", rt_settings_path);
    f = fopen(tmp, "w");
    if (f) {
        for (int l = 0; l < n; l++)
            fprintf(f, "%s\n", lines[l]);
        if (fclose(f) == 0)
            rename(tmp, rt_settings_path);
    } else
        fprintf(stderr, "settings: %s can't be written\n", rt_settings_path);
    for (int l = 0; l < n; l++)
        free(lines[l]);
    free(lines);
}

void settings_frame(unsigned long frame) {
    static const char *test;
    static int read;
    static unsigned long done_to;
    if (!read) {
        read = 1;
        test = getenv("MHFU_TEST_SETTING");
    }
    if (!test)
        return;
    for (const char *p = test; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        unsigned long at;
        char key[64], value[64];
        value[0] = 0;
        if (sscanf(p, "%lu,%63[^=;]=%63[^;]", &at, key, value) >= 2 && at <= frame && at > done_to) {
            if (!strcmp(key, "save"))       /* ("frame,save=1": written to settings.ini, as closing the screen does) */
                settings_save();
            else
                settings_set(key, value);
        }
    }
    done_to = frame;
}
