/* Mouse camera: the mouse turns and pitches the game's camera, by writing its yaw and height before each game frame,
 * and - with MHFU_FPS - at the display's rate, by turning the view of each frame shown.
 *
 * The game's scene (at *0x089CBD04) has six camera views of 0x1A0 bytes from +0xB0, the one in use numbered by the
 * byte at +0xA8D: 0 the hunt's, 2 the village's and the camp tent's (fixed angles, whatever the yaw says). At +0x120
 * are two 16-bit angles (65536 a turn), the yaw it is turning to and, at +0x122, the yaw it has: the bearing of the
 * eye from the target. The camera eases from one to the other, so turning both turns it at once. ← on the D-pad turns
 * it left, raising the yaw. The camera the frame was drawn with is the scene's: eye at +0xB40, target at +0xB50, view
 * matrix at +0xB80.
 *
 * Its height is a preset, pointed to from +0x110 (↑ and ↓ step through a table of them): floats at +4, the eye's height
 * over the hunter, +8 its distance and +0x10 the height it looks at. Moving the mouse up and down pitches the preset in
 * use: its eye moves around the point it looks at, keeping its distance, from 20 degrees below to 80 above; the camera
 * follows within a frame or two.
 *
 * At the display's rate the frames shown between the game's are interpolated (gpu.c), the camera with them; the
 * mouse's turn (each axis: yaw, the eye's bearing, and pitch, its elevation) is added at once instead - the part of it
 * the latest frame has that the interpolation has not shown yet (applied), what the game has been given for its next
 * frame (written), and what the mouse has moved since (unread):
 *     applied * (1 - t) + written + unread
 * which is continuous across the game's frames: just before one, the latest frame turned by written + unread; just
 * after, the frame before it (the same) turned by applied (that written) + written (that unread). As far as the game
 * follows: where the scenery holds the camera back it turns less, and showing the mouse's turn ahead of it would jerk
 * back every frame. A jump left at a game frame (the game did something else) fades over 60 ms.
 *
 * MHFU_MOUSE_SENS: degrees a mouse count turns (default 0.15). */
#include "hle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void window_mouse_peek(float *dx, float *dy);

#define SCENE_PTR 0x089CBD04u
#define PI 3.14159265f

static float sens = -1;
static float left;                      /* yaw not yet written (in 65536ths) */
static unsigned long tick;              /* the game's frames, counted */
static int active;                      /* the camera is the hunt's, which the mouse turns */

/* An axis the mouse turns: the game's angle in its last two frames; the turn the latest was made with (applied) and
 * the one given for the next (written), radians; r: how much of its last turn the game did; the angle shown just
 * before the latest frame, from the frame before it (to carry the picture across the frame); and a jump being faded */
typedef struct {
    float angle[2], applied, written, r, before, before_r, corr;
    uint64_t corr_at;
    unsigned long seen;
} Axis;
static Axis yaw_axis = {.r = 1, .before_r = 1}, pitch_axis = {.r = 1, .before_r = 1};

/* The game's last two frames' cameras: view matrices (4x3, as gpu.c's), targets, eyes; have: how many are recorded */
static float views[2][12], targets[2][3], eyes[2][3];
static int have;

static float wrap(float a) {
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

static int in_ram(uint32_t a) {
    return a >= 0x08800000u && a < 0x0A000000u;
}

/* A game frame: the axis's angle in it */
static void axis_frame(Axis *x, float angle) {
    x->before = x->angle[1];
    x->before_r = x->r;
    x->applied = x->written;
    x->written = 0;
    x->angle[0] = x->angle[1];
    x->angle[1] = angle;
    if (fabsf(x->applied) > 0.002f) {
        float f = wrap(x->angle[1] - x->angle[0]) / x->applied;
        x->r = f < 0 ? 0 : f > 1 ? 1 : f;
    }
}

/* The turn to show on top of the latest frame, t of the way from the one before, unread: the mouse's since the game
 * took it */
static float axis_shown(Axis *x, float t, int interpolating, float unread) {
    float turned = wrap(x->angle[1] - x->angle[0]), pending = x->written + unread;
    float a = interpolating ? x->r * ((1 - t) * x->applied + pending) - (1 - t) * turned : x->r * pending;
    uint64_t now = time_us();
    float fade = x->corr * expf(-(float)(now - x->corr_at) / 60000.0f);
    if (x->seen != tick) {
        x->seen = tick;
        float jump = wrap(x->angle[1] + a - (x->before + x->before_r * (x->applied + pending)));
        fade = fabsf(jump) < PI / 6 ? fade - jump : 0;      /* over 30 degrees: a cut, shown as it is */
        x->corr = fade;
        x->corr_at = now;
    }
    return a + fade;
}

/* The presets the mouse has pitched: the eye's distance from where it looks, its angle above it, and what was last
 * written (if the game has written something else since, it is the game's preset again) */
static struct { uint32_t addr; float radius, angle, height, distance; } pitched[16];

/* Pitch the preset by `by` (radians, up); returns how much it pitched (less at the limits) */
static float pitch(uint32_t preset, float by) {
    float *height = (float *)MEM(preset + 4), *distance = (float *)MEM(preset + 8), look = *(float *)MEM(preset + 0x10);
    if (!(*distance > 0 && *distance < 5000))
        return 0;
    int i = 0;
    while (i < 16 && pitched[i].addr && pitched[i].addr != preset)
        i++;
    if (i == 16)
        i = 0;
    if (pitched[i].addr != preset || pitched[i].height != *height || pitched[i].distance != *distance) {
        float up = *height - look;
        pitched[i].addr = preset;
        pitched[i].radius = sqrtf(up * up + *distance * *distance);
        pitched[i].angle = atan2f(up, *distance);
    }
    float a = pitched[i].angle + by, lo = -20 * PI / 180, hi = 80 * PI / 180;
    a = a < lo ? lo : a > hi ? hi : a;
    float done = a - pitched[i].angle;
    pitched[i].angle = a;
    *height = pitched[i].height = look + pitched[i].radius * sinf(a);
    *distance = pitched[i].distance = pitched[i].radius * cosf(a);
    return done;
}

/* The sensitivity (degrees a mouse count turns): a recording keeps the one it was made with, for its replay */
float camera_sens(void) {
    if (sens < 0) {
        const char *e = getenv("MHFU_MOUSE_SENS");
        sens = e ? (float)atof(e) : 0.15f;
    }
    return sens;
}

void camera_set_sens(float s) {
    sens = s;
}

/* After each game frame: note its camera, then give the game the mouse's motion (this frame's, dx dy) for the next */
void camera_update(float dx, float dy) {
    camera_sens();
    tick++;
    active = 0;
    uint32_t scene = *(uint32_t *)MEM(SCENE_PTR);
    uint8_t view_no = in_ram(scene) ? *MEM(scene + 0xA8D) : 0xFF;
    if (view_no >= 6) {
        yaw_axis.written = pitch_axis.written = 0;
        return;
    }
    uint32_t view = scene + 0xB0 + view_no * 0x1A0u;
    uint16_t *yaw = (uint16_t *)MEM(view + 0x120);
    /* the frame's camera (its view matrix 4x4 by columns: the game multiplies it into each world matrix) */
    const float *e = (const float *)MEM(scene + 0xB40), *t = (const float *)MEM(scene + 0xB50);
    const float *vm = (const float *)MEM(scene + 0xB80);
    memcpy(views[0], views[1], sizeof views[0]);
    memcpy(targets[0], targets[1], sizeof targets[0]);
    memcpy(eyes[0], eyes[1], sizeof eyes[0]);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 3; i++)
            views[1][3 * j + i] = vm[4 * j + i];
    memcpy(targets[1], t, sizeof targets[1]);
    memcpy(eyes[1], e, sizeof eyes[1]);
    if (have < 2)
        have++;
    float ox = e[0] - t[0], oy = e[1] - t[1], oz = e[2] - t[2];
    axis_frame(&yaw_axis, atan2f(ox, oz));
    axis_frame(&pitch_axis, atan2f(oy, sqrtf(ox * ox + oz * oz)));
    /* the hunt's camera turns with the yaw; the others keep their own angles */
    active = view_no == 0;
    /* moving the mouse right turns the camera right: the yaw goes down */
    left += -dx * sens * 65536.0f / 360.0f;
    int turn = (int)left;
    if (turn) {
        left -= (float)turn;
        yaw[0] = (uint16_t)(yaw[0] + turn);
        yaw[1] = (uint16_t)(yaw[1] + turn);
        yaw_axis.written = (float)turn * 2 * PI / 65536;
    }
    /* the pitch: moving the mouse up looks up, the eye going down around the hunter at the same distance */
    uint32_t preset = *(uint32_t *)MEM(view + 0x110);
    if (active && dy != 0 && in_ram(preset))
        pitch_axis.written = pitch(preset, dy * sens * PI / 180.0f);
}

/* a o b, 4x3 matrices (as gpu.c's: columns, the last the translation) */
static void compose(const float *a, const float *b, float *out) {
    float m[12];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 3; i++)
            m[3 * j + i] = a[i] * b[3 * j] + a[3 + i] * b[3 * j + 1] + a[6 + i] * b[3 * j + 2] + (j == 3 ? a[9 + i] : 0);
    memcpy(out, m, sizeof m);
}

/* The inverse of a rigid 4x3 transform: R^T, -R^T T */
static void invert(const float *v, float *out) {
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++)
            out[3 * j + i] = v[3 * i + j];
    for (int i = 0; i < 3; i++)
        out[9 + i] = -(v[3 * i] * v[9] + v[3 * i + 1] * v[10] + v[3 * i + 2] * v[11]);
}

/* A rotation by angle about the unit axis k (3x3, by columns) */
static void rotation(const float *k, float angle, float *m) {
    float c = cosf(angle), s = sinf(angle);
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++)
            m[3 * j + i] = (1 - c) * k[i] * k[j] + (i == j ? c : 0);
    /* + sin [k]x: column j, row i: s * (k x e_j)_i */
    m[3 * 0 + 1] += s * k[2];  m[3 * 0 + 2] -= s * k[1];
    m[3 * 1 + 0] -= s * k[2];  m[3 * 1 + 2] += s * k[0];
    m[3 * 2 + 0] += s * k[1];  m[3 * 2 + 1] -= s * k[0];
}

static void mul33(const float *a, const float *b, float *out) {
    float m[9];
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++)
            m[3 * j + i] = a[i] * b[3 * j] + a[3 + i] * b[3 * j + 1] + a[6 + i] * b[3 * j + 2];
    memcpy(out, m, sizeof m);
}

/* For showing a frame t of the way from the game's previous frame to its latest: the two frames' cameras undone (the
 * inverses of their views, to take each world matrix back to the world) and the camera to show it through. Returns 0
 * if the camera is not the mouse's (gpu.c then interpolates the matrices as they are).
 *
 * The camera shown is the latest's, its target moved to where the target is at t, and its eye turned about it by the
 * yaw and pitch at t (as above). Across a cut (the camera jumped) it is the latest's, with what is pending. */
int camera_views(float t, int interpolating, float *prev_inv, float *latest_inv, float *shown) {
    if (!active || have < 2 || sens < 0)
        return 0;
    invert(views[0], prev_inv);
    invert(views[1], latest_inv);
    float moved = sqrtf((eyes[1][0] - eyes[0][0]) * (eyes[1][0] - eyes[0][0]) +
                        (eyes[1][1] - eyes[0][1]) * (eyes[1][1] - eyes[0][1]) +
                        (eyes[1][2] - eyes[0][2]) * (eyes[1][2] - eyes[0][2]));
    if (fabsf(wrap(yaw_axis.angle[1] - yaw_axis.angle[0])) > PI / 3 || moved > 600)
        interpolating = 0;
    float dx, dy;
    window_mouse_peek(&dx, &dy);
    float a = axis_shown(&yaw_axis, t, interpolating, (left - dx * sens * 65536.0f / 360.0f) * 2 * PI / 65536);
    float b = axis_shown(&pitch_axis, t, interpolating, dy * sens * PI / 180.0f);
    /* MHFU_CAMERA_LOG=file: each frame shown, its time, t and the yaw and pitch shown (for checking smoothness) */
    static FILE *log;
    static int log_read;
    if (!log_read) {
        log_read = 1;
        if (getenv("MHFU_CAMERA_LOG"))
            log = fopen(getenv("MHFU_CAMERA_LOG"), "w");
    }
    if (log)
        fprintf(log, "%llu %.4f %.6f %d %.6f\n", (unsigned long long)time_us(), t, yaw_axis.angle[1] + a, interpolating,
                pitch_axis.angle[1] + b);
    float c[3];
    for (int i = 0; i < 3; i++)
        c[i] = interpolating ? targets[0][i] + (targets[1][i] - targets[0][i]) * t : targets[1][i];
    /* The eye's offset from the target turned by M: by a about the vertical, then up by b about the horizontal at right
     * angles to it (k: rotating about k by -b raises the offset (sin, 0, cos) of bearing ψ). The camera shown sees a
     * point x as the latest sees M^T (x - c) + target. */
    const float up[3] = {0, 1, 0};
    float yaw_m[9], pitch_m[9], m[9];
    rotation(up, a, yaw_m);
    float psi = yaw_axis.angle[1] + a;
    const float k[3] = {cosf(psi), 0, -sinf(psi)};
    rotation(k, -b, pitch_m);
    mul33(pitch_m, yaw_m, m);
    float q[12];
    for (int j = 0; j < 3; j++)                 /* M^T */
        for (int i = 0; i < 3; i++)
            q[3 * j + i] = m[3 * i + j];
    for (int i = 0; i < 3; i++)
        q[9 + i] = targets[1][i] - (q[i] * c[0] + q[3 + i] * c[1] + q[6 + i] * c[2]);
    compose(views[1], q, shown);
    return 1;
}
