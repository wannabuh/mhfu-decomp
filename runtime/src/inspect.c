/* The click inspector (F7 opens and closes) and the hold (F8, with or without it: the game stopped, its last tick shown; "."
 * lets one tick through; F6 captures what is shown, the game going on to where its state can be taken, then held again;
 * MHFU_TEST_STEP="every,count" steps for a test; MHFU_TEST_INSPECT="frame,x,y;..." clicks at
 * those PSP pixels, MHFU_TEST_PAUSE="frame,n" holds the game there for n passes). The mouse is let go of, as a pointer,
 * and is not the game's while it is open: keys and the pad still are.
 *
 * A click: the nearest object on the screen, by projecting each object's place (+0x200) with the camera shown (gpu.c: not
 * always the game's: camera.c) and taking the nearest to the click, then the next nearest on another click at the same
 * spot. So it is by each object's feet and the middle of its height, not its shape: a big monster clicked far from its feet
 * can lose to something nearer the click. A panel shows the object live, and a marker where it is.
 *
 * Under the pointer: the draws of the frame shown that cover that pixel (gpu.c gpu_pick_at), the top one first, with
 * their textures: hash, as MHFU_TEX_HIDE and texture packs name it, size, address. How exact: their triangles as drawn, near
 * clipped, culled, scissored and depth tested in draw order for that one pixel, in the render target shown. Not the texels:
 * a hole in a texture (alpha test) or a colour mask is not seen, which is why several draws are listed, not one.
 *
 * Reads only: the game's memory is read, nothing else of the game's is touched. The selection goes to inspect.txt in the
 * captures' folder, and into a capture's info.txt (capture.c). Class names and species are shown with their confidence. */
#include "inspect.h"
#include "hitview.h"
#include "capture.h"
#include "objects.h"
#include "hle.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int window_point(float wx, float wy, float *x, float *y);
int window_mouse_position(float *x, float *y);
void window_inspect_mode(int on);
void window_note(const char *text);
unsigned long present_frame_number(void);
uint64_t wall_time_us(void);

#define MAX_OBJECTS 2048
#define MAX_HITS 6

static int open, paused;
static long test_pause_left;
static float pointer_x = 240, pointer_y = 136, click_x = -1000, click_y = -1000;
static uint32_t selected, selected_vtable;
static int selected_list, selected_rank, candidates;
static GpuPickHit hits[MAX_HITS];
static int nhits;
static unsigned hits_generation;
static float hits_x = -1, hits_y = -1;
static int write_due;
static uint64_t last_request;

int inspect_open(void) { return open; }
static int step_due;                /* ticks to let through while held (the . key) */
static long test_step_every, test_steps_left, test_step_count;

int inspect_paused(void) {
    if (test_pause_left > 0 && --test_pause_left == 0)
        paused = 0;
    if (paused && test_steps_left > 0 && ++test_step_count >= test_step_every) {
        test_step_count = 0;
        test_steps_left--;
        step_due = 1;
    }
    if (paused && step_due) {
        step_due = 0;
        gpu_pick_request();
        return 0;
    }
    return paused;
}

void inspect_toggle_pause(void) {
    paused = !paused;
    step_due = 0;
    gpu_pick_request();
    window_note(paused ? "game held: f8 lets go, . a tick on, f6 captures" : "game running");
}

void inspect_step(void) {
    if (paused)
        step_due = 1;
}

void inspect_toggle(void) {
    open = !open;
    gpu_pick_enable(open || hit_view_on);
    window_inspect_mode(open);
    if (open) {
        float wx, wy, x, y;
        if (window_mouse_position(&wx, &wy) && window_point(wx, wy, &x, &y))
            pointer_x = x, pointer_y = y;
        gpu_pick_request();
    }
}

/* -- the selection -------------------------------------------------------------------------------------------------- */

typedef struct { uint32_t addr, vtable; int list; float d; } Candidate;

static int cmp_candidate(const void *a, const void *b) {
    float x = ((const Candidate *)a)->d, y = ((const Candidate *)b)->d;
    return x < y ? -1 : x > y;
}

/* How far a point on the screen is from the object: from its feet, or the middle of its height (as a guess at it, by its
 * scale: a character about 100 units up, a large monster more); so a click on the body finds it */
static float distance_to(const GameObject *o, float x, float y) {
    float best = 1e9f, sx, sy;
    float scale = 1;
    memcpy(&scale, MEM(o->addr + 0x224), sizeof scale);     /* (the model's scale +0x220, its y) */
    if (!(scale > 0.1f && scale < 20))
        scale = 1;
    for (int i = 0; i < 2; i++) {
        float p[3] = {o->place[0], o->place[1] + (i ? 90.0f * scale : 0), o->place[2]};
        if (gpu_pick_project(p, &sx, &sy)) {
            float d = hypotf(sx - x, sy - y);
            if (d < best)
                best = d;
        }
    }
    return best;
}

static int click_pending;
static float pending_x, pending_y;

static void click(float x, float y) {
    float ignored_x, ignored_y;
    static int waited;
    if (!gpu_pick_project((float[3]){0, 0, 0}, &ignored_x, &ignored_y) && !gpu_pick_project((float[3]){0, 0, 1000}, &ignored_x, &ignored_y) &&
        waited < 30) {
        waited++;
        click_pending = 1;              /* (no camera yet: as soon as a frame has been drawn; a screen with no 3D, no objects to find) */
        pending_x = x, pending_y = y;
        pointer_x = x, pointer_y = y;
        return;
    }
    click_pending = 0;
    waited = 0;
    static GameObject all[MAX_OBJECTS];
    static Candidate c[MAX_OBJECTS];
    int n = objects_read(all, MAX_OBJECTS), m = 0;
    for (int i = 0; i < n; i++) {
        float d = distance_to(&all[i], x, y);
        if (d < 1e8f)
            c[m++] = (Candidate){all[i].addr, all[i].vtable, all[i].list, d};
    }
    qsort(c, (size_t)m, sizeof *c, cmp_candidate);
    int same_spot = hypotf(x - click_x, y - click_y) <= 4;
    click_x = x, click_y = y;
    pointer_x = x, pointer_y = y;
    int rank = same_spot && m ? (selected_rank + 1) % m : 0;
    candidates = m;
    selected_rank = rank;
    if (m) {
        selected = c[rank].addr;
        selected_vtable = c[rank].vtable;
        selected_list = c[rank].list;
    } else
        selected = 0;
    gpu_pick_request();
    write_due = 1;
}

int inspect_event(const SDL_Event *e) {
    if (!open)
        return 0;
    float x, y;
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (window_point(e->motion.x, e->motion.y, &x, &y))
            pointer_x = x, pointer_y = y;
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT && window_point(e->button.x, e->button.y, &x, &y))
            click(x, y);
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return 1;
    }
    return 0;
}

static void update_hits(void);

/* -- each game frame ------------------------------------------------------------------------------------------------- */

void inspect_tick(unsigned long frame) {
    static int read;
    static struct { unsigned long frame; float x, y; } test[32];
    static int ntest;
    static unsigned long pause_at;
    static long pause_for;
    if (!read) {
        read = 1;
        const char *e = getenv("MHFU_TEST_INSPECT");
        for (; e && *e && ntest < 32;) {
            int n = 0;
            if (sscanf(e, "%lu,%f,%f%n", &test[ntest].frame, &test[ntest].x, &test[ntest].y, &n) != 3)
                break;
            ntest++;
            e += n;
            if (*e == ';')
                e++;
        }
        e = getenv("MHFU_TEST_STEP");       /* every,count: a tick let through every that many passes, while held */
        if (e)
            sscanf(e, "%ld,%ld", &test_step_every, &test_steps_left);
        e = getenv("MHFU_TEST_PAUSE");
        if (e)
            sscanf(e, "%lu,%ld", &pause_at, &pause_for);
    }
    for (int i = 0; i < ntest; i++)
        if (test[i].frame == frame) {
            if (!open)
                inspect_toggle();
            click(test[i].x, test[i].y);
        }
    if (pause_for && frame == pause_at) {
        if (!open && !getenv("MHFU_TEST_PAUSE_PLAIN"))
            inspect_toggle();
        paused = 1;
        test_pause_left = pause_for;
    }
    if (!open)
        return;
    if (click_pending)
        click(pending_x, pending_y);
    update_hits();
    uint64_t now = wall_time_us();
    if (now - last_request > 200000) {  /* (five times a second is plenty for what is under a pointer) */
        last_request = now;
        gpu_pick_request();
    }
}

/* -- what is shown ---------------------------------------------------------------------------------------------------- */

static void update_hits(void) {
    unsigned g = gpu_pick_generation();
    if (g == hits_generation && pointer_x == hits_x && pointer_y == hits_y)
        return;
    int fresh = g != hits_generation;
    hits_generation = g;
    hits_x = pointer_x, hits_y = pointer_y;
    nhits = gpu_pick_at(pointer_x, pointer_y, hits, MAX_HITS);
    if (write_due && fresh) {
        write_due = 0;
        char path[4200];
        snprintf(path, sizeof path, "%s/inspect.txt", capture_dir());
        FILE *f = fopen(path, "w");
        if (f) {
            inspect_write(f);
            fclose(f);
        }
    }
}

static void hit_flags(const GpuPickHit *h, char *t, size_t n) {
    snprintf(t, n, "%s%s%s", h->through ? "2D" : "3D", h->blend ? " BLEND" : "", h->alpha_test ? " ATEST" : "");
}

static const char *format_name(int fmt) {
    static const char *names[] = {"565", "5551", "4444", "8888", "I4", "I8", "I16", "I32", "DXT1", "DXT3", "DXT5"};
    return fmt >= 0 && fmt < 11 ? names[fmt] : "?";
}

void inspect_write(FILE *f) {
    GameObject o;
    if (selected && object_read(selected, selected_list, &o) && o.vtable == selected_vtable) {
        fprintf(f, "object (rank %d of %d nearest the click at %.1f, %.1f): ", selected_rank + 1, candidates, (double)click_x, (double)click_y);
        object_print(f, &o);
    } else
        fprintf(f, "object: none selected\n");
    update_hits();
    fprintf(f, "under the pointer at %.1f, %.1f (draws covering that pixel, top first; triangles and depth exact, texels not looked at):\n",
            (double)pointer_x, (double)pointer_y);
    for (int i = 0; i < nhits; i++) {
        const GpuPickHit *h = &hits[i];
        char flags[32];
        hit_flags(h, flags, sizeof flags);
        fprintf(f, "  draw %d type %d count %d vertices %08X %s z %.0f blend %06X", h->draw, h->type, h->count, h->vaddr, flags, (double)h->z, h->blend_reg);
        if (h->textured)
            fprintf(f, " texture %08X %dx%d at %08X format %s\n", h->hash, h->w, h->h, h->addr, format_name(h->fmt));
        else
            fprintf(f, " no texture\n");
    }
    if (!nhits)
        fprintf(f, "  none (or no frame gone over yet)\n");
}

#define PANEL_DOTS 256
#define LINE 9

int inspect_rects(float x0, float y0, float scale, HintRect *out, int max) {
    if (!open) {
        if (!paused)
            return 0;
        /* (held without the inspector: only a mark that it is) */
        int d = (int)(scale / 2) < 1 ? 1 : (int)(scale / 2);
        char line[64];
        snprintf(line, sizeof line, "HELD %lu  F8 RUN  . STEP  F6 CAPTURE", present_frame_number());
        int w = pen_text_width(line) + 6;
        Pen m = {out, 0, max, d, (int)(x0 + 240 * scale) - w * d / 2, (int)y0 + 3 * d};
        pen_box(&m, 0, 0, w, 13, C_INK);
        pen_text(&m, 3, 3, line, C_LIT);
        return m.n;
    }
    if (click_pending)
        click(pending_x, pending_y);
    update_hits();
    int dot = (int)(scale / 2) < 1 ? 1 : (int)(scale / 2);
#define SX(v) ((int)(x0 + (float)(v) * scale))
#define SY(v) ((int)(y0 + (float)(v) * scale))
    Pen p = {out, 0, max, dot, 0, 0};
    GameObject o;
    int have = selected && object_read(selected, selected_list, &o) && o.vtable == selected_vtable;
    /* the marker: where the selected object's feet are, in corners, with a line up to its middle */
    float sx, sy;
    int on_screen = have && gpu_pick_project(o.place, &sx, &sy);
    if (on_screen) {
        p.x = SX(sx), p.y = SY(sy);
        const int a = 7, l = 3;
        for (int s = -1; s <= 1; s += 2) {
            pen_box(&p, s < 0 ? -a : a - l + 1, -a, l, 1, C_YELLOW);
            pen_box(&p, s < 0 ? -a : a - l + 1, a, l, 1, C_YELLOW);
            pen_box(&p, s < 0 ? -a : a, -a, 1, l, C_YELLOW);
            pen_box(&p, s < 0 ? -a : a, a - l + 1, 1, l, C_YELLOW);
        }
        pen_box(&p, 0, 0, 1, 1, C_YELLOW);
    }
    /* the pointer */
    p.x = SX(pointer_x), p.y = SY(pointer_y);
    pen_box(&p, -3, 0, 7, 1, C_WHITE);
    pen_box(&p, 0, -3, 1, 7, C_WHITE);
    /* the panel: lower left */
    int lines = 11 + (nhits ? nhits : 1), height = lines * LINE + 6;
    p.x = (int)x0 + 3 * dot;
    p.y = (int)(y0 + 272 * scale) - (height + 3) * dot;
    pen_box(&p, 0, 0, PANEL_DOTS, height, C_INK);
    char t[64];
    int y = 3;
#define TEXT(color, ...) do { snprintf(t, sizeof t, __VA_ARGS__); pen_text(&p, 3, y, t, color); y += LINE; } while (0)
    TEXT(C_LIT, "INSPECT  F7 CLOSE  F8 %s  CLICK: NEXT", paused ? "RUN" : "HOLD");
    if (have) {
        TEXT(C_WHITE, "OBJECT %d/%d  %08X  LIST %d", selected_rank + 1, candidates, o.addr, o.list);
        TEXT(C_WHITE, "CLASS %s", o.cls ? o.cls : "(UNNAMED)");
        TEXT(C_GREY, " (%s)  VT %08X", o.conf ? o.conf : "NO NAME", o.vtable);
        TEXT(C_WHITE, "KIND %d %s  TYPE 0X%02X", o.kind, object_kind_name(o.kind), o.type);
        TEXT(C_WHITE, "SPECIES %s", o.species ? o.species : "-");
        TEXT(C_GREY, " %s", o.species ? "(INFERRED, BY TYPE BYTE)" : "");
        TEXT(C_WHITE, "PLACE %.0f %.0f %.0f %s", (double)o.place[0], (double)o.place[1], (double)o.place[2], on_screen ? "" : "(OFF SCREEN)");
        TEXT(C_WHITE, "STATE %d/%d  MOTION 0X%X", o.group, o.state, o.motion);
        TEXT(C_WHITE, "TICKS %s", object_ticks_text(&o));
        if (o.has_health)
            TEXT(C_WHITE, "HEALTH %d/%d  AREA %d", o.health, o.health_max, o.area);
        else
            TEXT(C_GREY, "NO HEALTH READ  AREA %d", o.area);
    } else {
        TEXT(C_GREY, selected ? "THE OBJECT IS GONE" : "CLICK SOMETHING");
        y += 9 * LINE;
    }
    for (int i = 0; i < nhits; i++) {
        char flags[32];
        hit_flags(&hits[i], flags, sizeof flags);
        if (hits[i].textured)
            TEXT(i ? C_GREY : C_FACE, "%08X %dX%d @%08X %s", hits[i].hash, hits[i].w, hits[i].h, hits[i].addr, flags);
        else
            TEXT(i ? C_GREY : C_FACE, "NO TEXTURE  %s", flags);
    }
    if (!nhits)
        TEXT(C_GREY, "NOTHING DRAWN KNOWN HERE");
#undef TEXT
#undef SX
#undef SY
    return p.n;
}
