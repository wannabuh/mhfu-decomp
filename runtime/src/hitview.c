/* The hit overlay (F12; MHFU_TEST_HITVIEW=1: from the start): what the game's hit test tested, drawn over the picture and
 * logged. It shows only what the test really used, told by readable/hit_detect.c as it goes, so with MHFU_HAND=0 (the
 * generated code in its place) it shows nothing, and a monster's shapes only while an attack is being tested against it.
 *
 *   red      an attack's shapes, over its last ticks (the swing's path)
 *   green    a monster's part that could take this hit, in the last test
 *   grey     a part that couldn't (the part's and record's bits: monster_part_can_take)
 *   yellow   the pair that touched, held for two seconds
 *
 * A panel says what the last attack did. MHFU_HIT_LOG=<file>, or hits.log in the captures' folder from F12: each test,
 * with the gap between the two shapes worked out here (negative: overlapping), for telling "the shapes miss" from "they
 * overlap and nothing lands". Shapes are where the game has them at its tick, and the picture shows the game a little
 * behind, so a fast swing is drawn ahead of the model.
 *
 * Reads only: nothing of the game's is written or called. */
#include "hitview.h"
#include "capture.h"
#include "hle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gpu_pick_project(const float *p, float *x, float *y);
void gpu_pick_enable(int on);
int inspect_open(void);

int hit_view_on;

typedef struct { int sphere; float a[3], b[3], r; } Shape;

enum { ROLE_ATTACK, ROLE_PART, ROLE_TOUCHED, ROLE_PART_NOT };
typedef struct { Shape s; int role; unsigned long tick; } Kept;

#define MAX_KEPT 2048
#define ATTACK_TICKS 40         /* how long an attack's shapes stay */
#define PART_TICKS 120
static Kept kept[MAX_KEPT];
static int nkept;
static unsigned long tick_now, part_tick;
static FILE *log_file;
static int log_tried;

static struct {
    int have;
    unsigned kind, damage, hit_id;
    int shapes, tested, touched, skip_why;
    uint32_t target;
    int part_id;
    float nearest;
    unsigned long at;
    int hit_part;
    unsigned long hit_at;
} last;

static const char *skip_names[HIT_SKIPS] = {"OTHER AREA", "RECORD NOT FOR TARGETS LIKE THIS", "ITS OWNER", "ALREADY LOGGED THIS HIT",
                                            "ALREADY HIT THIS TICK", "LAST HIT RECORD STILL SET", "KIND RULES", "HIT IMMUNE"};

static float read_f(uint32_t a) {
    uint32_t u = *(uint32_t *)MEM(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* The shape's forms as collision_shape_from_bone_record (game_sub 09C37898) writes them: type 0 two ends +0x10, +0x20 and
 * the radius +0x30; otherwise a centre +0x10 and the radius +0x20 */
static Shape read_shape(uint32_t at) {
    Shape s;
    s.sphere = *(uint32_t *)MEM(at) != 0;
    for (int i = 0; i < 3; i++) {
        s.a[i] = read_f(at + 0x10 + 4 * (uint32_t)i);
        s.b[i] = s.sphere ? s.a[i] : read_f(at + 0x20 + 4 * (uint32_t)i);
    }
    s.r = read_f(at + (s.sphere ? 0x20u : 0x30u));
    return s;
}

/* The least distance between two segments (Ericson's, Real-Time Collision Detection) */
static float segments_apart(const float *p1, const float *q1, const float *p2, const float *q2) {
    float d1[3], d2[3], r[3];
    for (int i = 0; i < 3; i++) {
        d1[i] = q1[i] - p1[i];
        d2[i] = q2[i] - p2[i];
        r[i] = p1[i] - p2[i];
    }
#define DOT(x, y) ((x)[0] * (y)[0] + (x)[1] * (y)[1] + (x)[2] * (y)[2])
    float a = DOT(d1, d1), e = DOT(d2, d2), f = DOT(d2, r), s = 0, t = 0;
    if (a > 1e-6f || e > 1e-6f) {
        if (a <= 1e-6f) {
            t = fminf(fmaxf(f / e, 0), 1);
        } else {
            float c = DOT(d1, r);
            if (e <= 1e-6f) {
                s = fminf(fmaxf(-c / a, 0), 1);
            } else {
                float b = DOT(d1, d2), den = a * e - b * b;
                s = den > 1e-6f ? fminf(fmaxf((b * f - c * e) / den, 0), 1) : 0;
                t = (b * s + f) / e;
                if (t < 0) {
                    t = 0;
                    s = fminf(fmaxf(-c / a, 0), 1);
                } else if (t > 1) {
                    t = 1;
                    s = fminf(fmaxf((b - c) / a, 0), 1);
                }
            }
        }
    }
    float v[3];
    for (int i = 0; i < 3; i++)
        v[i] = p1[i] + d1[i] * s - (p2[i] + d2[i] * t);
    float dist = sqrtf(DOT(v, v));
#undef DOT
    return dist;
}

static float gap(const Shape *x, const Shape *y) {
    return segments_apart(x->a, x->b, y->a, y->b) - x->r - y->r;
}

static void keep(const Shape *s, int role) {
    if (nkept == MAX_KEPT) {
        memmove(kept, kept + MAX_KEPT / 4, sizeof *kept * (MAX_KEPT - MAX_KEPT / 4));
        nkept = MAX_KEPT - MAX_KEPT / 4;
    }
    kept[nkept++] = (Kept){*s, role, tick_now};
}

static char log_path[1024];

/* (capture.c) the log, if it's on */
const char *hit_view_log_path(void) { return log_path[0] ? log_path : NULL; }
void hit_view_log_flush(void) {
    if (log_file)
        fflush(log_file);
}

static void log_open(void) {
    if (log_tried)
        return;
    log_tried = 1;
    const char *e = getenv("MHFU_HIT_LOG");
    char path[1024];
    if (e)
        snprintf(path, sizeof path, "%s", e);
    else
        snprintf(path, sizeof path, "%s/hits.log", capture_dir());
    log_file = fopen(path, "a");
    if (log_file)
        snprintf(log_path, sizeof log_path, "%s", path);
    if (log_file)
        fprintf(log_file, "# tick record kind hit_id: A(ttack shape) ends radius | S(kipped) target why | P(art) target part_id bone_kind can_take "
                          "touched gap ends radius\n");
}

/* Only what hunters' attacks do: a monster's own are tested against hunters all the time */
static int hunters(uint32_t record) {
    return *(uint8_t *)MEM(record + 0x0F) == 0;
}

static unsigned rec_kind(uint32_t record) { return *(uint8_t *)MEM(record + 0x1F); }
static unsigned rec_id(uint32_t record) { return *(uint16_t *)MEM(record + 0x1A); }

void hit_view_skip(uint32_t record, uint32_t target, int why) {
    (void)target;
    if (!hunters(record))
        return;
    last.skip_why = why;
    last.target = target;
    last.at = tick_now;
    if (log_file)
        fprintf(log_file, "%lu %08X %X %X S %08X %s\n", tick_now, record, rec_kind(record), rec_id(record), target, skip_names[why]);
}

void hit_view_attacker(uint32_t record, uint32_t shape) {
    if (!hunters(record))
        return;
    Shape s = read_shape(shape);
    keep(&s, ROLE_ATTACK);
    if (last.at != tick_now || !last.have) {
        last.have = 1;
        last.tested = last.touched = last.shapes = 0;
        last.nearest = 1e30f;
        last.skip_why = -1;
        last.part_id = -1;
    }
    last.kind = rec_kind(record);
    last.damage = *(uint8_t *)MEM(record + 0x1E);
    last.hit_id = rec_id(record);
    last.shapes++;
    last.at = tick_now;
    if (log_file)
        fprintf(log_file, "%lu %08X %X %X A %.1f %.1f %.1f  %.1f %.1f %.1f  %.1f\n", tick_now, record, last.kind, last.hit_id, (double)s.a[0],
                (double)s.a[1], (double)s.a[2], (double)s.b[0], (double)s.b[1], (double)s.b[2], (double)s.r);
}

void hit_view_part(uint32_t record, uint32_t target, uint32_t part, uint32_t attacker_shape, uint32_t target_shape, int can_take, int touched) {
    if (!hunters(record))
        return;
    Shape a = read_shape(attacker_shape), t = read_shape(target_shape);
    float g = gap(&a, &t);
    int id = *(int16_t *)MEM(part);
    if (part_tick != tick_now) {        /* (only the last test's parts) */
        int n = 0;
        for (int i = 0; i < nkept; i++)
            if (kept[i].role != ROLE_PART && kept[i].role != ROLE_PART_NOT)
                kept[n++] = kept[i];
        nkept = n;
        part_tick = tick_now;
    }
    keep(&t, touched ? ROLE_TOUCHED : can_take ? ROLE_PART : ROLE_PART_NOT);
    last.tested++;
    last.target = target;
    if (can_take && g < last.nearest) {
        last.nearest = g;
        last.part_id = id;
    }
    if (touched) {
        last.touched++;
        last.hit_part = id;
        last.hit_at = tick_now;
        keep(&a, ROLE_TOUCHED);
    }
    if (log_file)
        fprintf(log_file, "%lu %08X %X %X P %08X %d %d %d %d %.1f  %.1f %.1f %.1f  %.1f %.1f %.1f  %.1f\n", tick_now, record, rec_kind(record),
                rec_id(record), target, id, *(int16_t *)MEM(part + 2), can_take, touched, (double)g, (double)t.a[0], (double)t.a[1], (double)t.a[2],
                (double)t.b[0], (double)t.b[1], (double)t.b[2], (double)t.r);
}

void hit_view_toggle(void) {
    hit_view_on = !hit_view_on;
    gpu_pick_enable(hit_view_on || inspect_open());
    if (hit_view_on)
        log_open();
    else if (log_file)
        fflush(log_file);
}

void hit_view_tick(unsigned long frame) {
    tick_now = frame;
    static int read;
    if (!read) {
        read = 1;
        if (getenv("MHFU_TEST_HITVIEW") && atoi(getenv("MHFU_TEST_HITVIEW")))
            hit_view_toggle();
    }
    if (!hit_view_on)
        return;
    int n = 0;
    for (int i = 0; i < nkept; i++) {
        unsigned long age = frame - kept[i].tick;
        if (age <= (kept[i].role == ROLE_ATTACK ? ATTACK_TICKS : PART_TICKS))
            kept[n++] = kept[i];
    }
    nkept = n;
    if (log_file && frame % 60 == 0)
        fflush(log_file);
}

/* -- drawing -------------------------------------------------------------------------------------------------------- */

static void line(Pen *p, float x0, float y0, float x1, float y1, int color, int size) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    int steps = (int)(len / (float)(size * 4)) + 1;     /* (dotted: the rectangles are shared with everything over the picture) */
    if (steps > 60)
        steps = 60;
    for (int i = 0; i <= steps && p->n < p->max; i += 1) {
        float t = (float)i / (float)steps;
        p->out[p->n++] = (HintRect){(int)(x0 + dx * t), (int)(y0 + dy * t), size, size, color};
    }
}

/* A sphere of radius r at c as seen: its size on the screen from how far r reaches, in a window's pixels */
static int screen_circle(const float *c, float r, float scale, float x0, float y0, float *sx, float *sy, float *sr) {
    float x, y;
    if (!gpu_pick_project(c, &x, &y))
        return 0;
    float most = 0;
    for (int axis = 0; axis < 3; axis++) {
        float q[3] = {c[0], c[1], c[2]}, qx, qy;
        q[axis] += r;
        if (gpu_pick_project(q, &qx, &qy)) {
            float d = (qx - x) * (qx - x) + (qy - y) * (qy - y);
            if (d > most)
                most = d;
        }
    }
    *sx = x0 + x * scale;
    *sy = y0 + y * scale;
    *sr = sqrtf(most) * scale;
    return 1;
}

static void circle(Pen *p, float x, float y, float r, int color, int size) {
    const int n = 16;
    for (int i = 0; i < n; i++) {
        float a = (float)i / (float)n * 6.2831853f, b = (float)(i + 1) / (float)n * 6.2831853f;
        line(p, x + r * cosf(a), y + r * sinf(a), x + r * cosf(b), y + r * sinf(b), color, size);
    }
}

static void draw_shape(Pen *p, const Shape *s, int color, int size, float scale, float x0, float y0) {
    float ax, ay, ar, bx, by, br;
    if (!screen_circle(s->a, s->r, scale, x0, y0, &ax, &ay, &ar))
        return;
    circle(p, ax, ay, ar, color, size);
    if (s->sphere || !screen_circle(s->b, s->r, scale, x0, y0, &bx, &by, &br))
        return;
    circle(p, bx, by, br, color, size);
    /* the two sides: out from the axis, square to it on the screen */
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    if (len < 1)
        return;
    float nx = -dy / len, ny = dx / len;
    line(p, ax + nx * ar, ay + ny * ar, bx + nx * br, by + ny * br, color, size);
    line(p, ax - nx * ar, ay - ny * ar, bx - nx * br, by - ny * br, color, size);
}

#define LINE 9

int hit_view_rects(float x0, float y0, float scale, HintRect *out, int max) {
    if (!hit_view_on)
        return 0;
    int dot = (int)(scale / 2) < 1 ? 1 : (int)(scale / 2);
    /* (a share of the rectangles, and what matters most first, so that the panel and what touched are always there; where
     * they overlap it doesn't matter which is on top) */
    Pen p = {out, 0, max / 2, dot, 0, 0};
    static const int order[] = {ROLE_TOUCHED, ROLE_ATTACK, ROLE_PART, ROLE_PART_NOT};
    static const int colors[] = {[ROLE_ATTACK] = C_RED, [ROLE_PART] = C_GREEN, [ROLE_TOUCHED] = C_YELLOW, [ROLE_PART_NOT] = C_GREY};
    for (int o = 0; o < 4; o++)
        for (int i = 0; i < nkept; i++)
            if (kept[i].role == order[o])
                draw_shape(&p, &kept[i].s, colors[kept[i].role], dot, scale, x0, y0);
    /* the panel: upper left */
    int lines = 7;
    p.max = max;
    p.x = (int)x0 + 3 * dot;
    p.y = (int)y0 + 3 * dot;
    pen_box(&p, 0, 0, 170, lines * LINE + 6, C_INK);
    char t[80];
    int y = 3;
#define TEXT(color, ...) do { snprintf(t, sizeof t, __VA_ARGS__); pen_text(&p, 3, y, t, color); y += LINE; } while (0)
    TEXT(C_LIT, "HITS  F12 CLOSE");
    if (!last.have) {
        TEXT(C_GREY, "NO ATTACK TESTED YET");
    } else {
        unsigned long age = tick_now - last.at;
        TEXT(age < 4 ? C_WHITE : C_GREY, "ATTACK KIND %X  DAMAGE %u  ID %X  %lu TICKS AGO", last.kind, last.damage, last.hit_id, age);
        TEXT(C_WHITE, "SHAPES %d  PARTS TESTED %d  TOUCHED %d", last.shapes, last.tested, last.touched);
        if (last.nearest < 1e29f)
            TEXT(last.nearest <= 0 ? C_YELLOW : C_WHITE, "NEAREST PART %d: GAP %.0f", last.part_id, (double)last.nearest);
        else
            TEXT(C_GREY, "NO PART COULD TAKE IT");
        if (last.skip_why >= 0)
            TEXT(C_GREY, "SKIPPED: %s", skip_names[last.skip_why]);
        else
            y += LINE;
        if (last.hit_at)
            TEXT(C_YELLOW, "LAST HIT: PART %d, %lu TICKS AGO", last.hit_part, tick_now - last.hit_at);
    }
#undef TEXT
    return p.n;
}
