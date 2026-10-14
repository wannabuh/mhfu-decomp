/* Mouse camera: the mouse turns the game's camera, by writing its yaw before each game frame, and - with MHFU_FPS - at
 * the display's rate, by turning the view of each frame shown.
 *
 * The game's scene (at *0x089CBD04) has six camera views of 0x1A0 bytes from +0xB0, the one in use numbered by the
 * byte at +0xA8D (0 in a quest, 2 in the village). A view's eye is at +0x00 and its target at +0x10; at +0x120 are
 * two 16-bit angles (65536 a turn), the yaw it is turning to and, at +0x122, the yaw it has. The camera eases from
 * one to the other, so turning both turns it at once. ← on the D-pad turns it left, raising the yaw. The camera the
 * frame was drawn with is the scene's: eye at +0xB40, target at +0xB50.
 *
 * Its height is a preset, pointed to from +0x110 (↑ and ↓ step through a table of them): floats at +4, the eye's height
 * over the hunter, +8 its distance and +0x10 the height it looks at. Moving the mouse up and down pitches the preset in
 * use: its eye moves around the point it looks at, keeping its distance, from 20 degrees below to 80 above; the camera
 * eases to it.
 *
 * At the display's rate: the frames shown between the game's are interpolated (gpu.c), the camera's yaw with them; the
 * mouse's turn is added at once instead - the part of it the latest frame has that the interpolation has not shown
 * yet (applied), what the game has been given for its next frame (written), and what the mouse has moved since. So
 * the view is turned about the camera's target by
 *     applied * (1 - t) + written + unread
 * which is continuous across the game's frames: just before one, the latest frame turned by written + unread; just
 * after, the frame before it (the same) turned by applied (that written) + written (that unread).
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
static float left;                      /* turn not yet written (in 65536ths) */

/* The yaw turn the latest was made with (radians); active: the camera follows the yaw (not in the village,
 * where it is fixed) */
static float applied, written;
static int active;

/* The game's last two frames' cameras: view matrices (4x3, as gpu.c's), targets, yaws (the eye's bearing from the
 * target, radians); have: how many frames are recorded (up to 2) */
static float views[2][12], targets[2][3], eyes[2][3], yaws[2];
static int have;

static int in_ram(uint32_t a) {
    return a >= 0x08800000u && a < 0x0A000000u;
}

/* The presets the mouse has pitched: the eye's distance from where it looks, its angle above it, and what was last
 * written (if the game has written something else since, it is the game's preset again) */
static struct { uint32_t addr; float radius, angle, height, distance; } pitched[16];

static void pitch(uint32_t preset, float by) {
    float *height = (float *)MEM(preset + 4), *distance = (float *)MEM(preset + 8), look = *(float *)MEM(preset + 0x10);
    if (!(*distance > 0 && *distance < 5000))
        return;
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
    pitched[i].angle = a;
    *height = pitched[i].height = look + pitched[i].radius * sinf(a);
    *distance = pitched[i].distance = pitched[i].radius * cosf(a);
}

/* After each game frame: note its camera, then give the game the mouse's motion (this frame's, dx dy) for the next */
void camera_update(float dx, float dy) {
    if (sens < 0) {
        const char *e = getenv("MHFU_MOUSE_SENS");
        sens = e ? (float)atof(e) : 0.15f;
    }
    applied = written;
    written = 0;
    active = 0;
    uint32_t scene = *(uint32_t *)MEM(SCENE_PTR);
    if (!in_ram(scene))
        return;
    uint8_t view_no = *MEM(scene + 0xA8D);
    if (view_no >= 6)
        return;
    uint32_t view = scene + 0xB0 + view_no * 0x1A0u;
    uint16_t *yaw = (uint16_t *)MEM(view + 0x120);
    /* the frame's view matrix (scene +0xB80, 4x4 by columns): the game multiplies it into each world matrix */
    const float *e = (const float *)MEM(scene + 0xB40), *t = (const float *)MEM(scene + 0xB50);
    const float *vm = (const float *)MEM(scene + 0xB80);
    memcpy(views[0], views[1], sizeof views[0]);
    memcpy(targets[0], targets[1], sizeof targets[0]);
    memcpy(eyes[0], eyes[1], sizeof eyes[0]);
    yaws[0] = yaws[1];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 3; i++)
            views[1][3 * j + i] = vm[4 * j + i];
    memcpy(targets[1], t, sizeof targets[1]);
    memcpy(eyes[1], e, sizeof eyes[1]);
    yaws[1] = atan2f(e[0] - t[0], e[2] - t[2]);
    if (have < 2)
        have++;
    /* the camera follows the yaw if it looks along it: its eye is behind the target, at the yaw half a turn round */
    int16_t off = (int16_t)((int)(atan2f(e[0] - t[0], e[2] - t[2]) * 32768 / PI) + 32768 - yaw[1]);
    /* and only the hunt's camera (view 0): the village's (2) has its own fixed angle, and eases back to it */
    active = view_no == 0 && off > -0x1000 && off < 0x1000;
    /* moving the mouse right turns the camera right: the yaw goes down */
    left += -dx * sens * 65536.0f / 360.0f;
    int turn = (int)left;
    if (turn) {
        left -= (float)turn;
        yaw[0] = (uint16_t)(yaw[0] + turn);
        yaw[1] = (uint16_t)(yaw[1] + turn);
        written = (float)turn * 2 * PI / 65536;
    }
    /* the pitch: moving the mouse up looks up, the eye going down around the hunter at the same distance */
    uint32_t preset = *(uint32_t *)MEM(view + 0x110);
    if (active && dy != 0 && in_ram(preset))
        pitch(preset, dy * sens * PI / 180.0f);
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

static float wrap(float a) {
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

/* For showing a frame t of the way from the game's previous frame to its latest: the two frames' cameras undone (world
 * to... the inverses of their views, to take each world matrix back to the world) and the camera to show it through.
 * Returns 0 if the camera is not the mouse's (gpu.c then interpolates the matrices as they are).
 *
 * The camera shown is the latest's, its target moved to where the target is at t, and turned about it to the yaw at
 * t: the yaw interpolated, with the mouse's part at once - applied (the latest's) not interpolated, and what is
 * pending added. Across a cut (the camera jumped) it is the latest's, with what is pending. */
int camera_views(float t, int interpolating, float *prev_inv, float *latest_inv, float *shown) {
    if (!active || have < 2 || sens < 0)
        return 0;
    invert(views[0], prev_inv);
    invert(views[1], latest_inv);
    float turned = wrap(yaws[1] - yaws[0]);
    float moved = sqrtf((eyes[1][0] - eyes[0][0]) * (eyes[1][0] - eyes[0][0]) +
                        (eyes[1][1] - eyes[0][1]) * (eyes[1][1] - eyes[0][1]) +
                        (eyes[1][2] - eyes[0][2]) * (eyes[1][2] - eyes[0][2]));
    if (fabsf(turned) > PI / 3 || moved > 600)
        interpolating = 0;
    float dx, dy;
    window_mouse_peek(&dx, &dy);
    /* pending: what the game has been given but not drawn yet (written), and what the mouse has moved since */
    float pending = written + (left - dx * sens * 65536.0f / 360.0f) * 2 * PI / 65536;
    float a = interpolating ? (1 - t) * (applied - turned) + pending : pending;
    float c[3];
    for (int i = 0; i < 3; i++)
        c[i] = interpolating ? targets[0][i] + (targets[1][i] - targets[0][i]) * t : targets[1][i];
    /* shown(x) = latest(Q^T (x - c) + target), Q the turn by a about the vertical: Q^T by columns (cos, 0, sin),
     * (0, 1, 0), (-sin, 0, cos) */
    float co = cosf(a), si = sinf(a);
    float q[12] = {co, 0, si, 0, 1, 0, -si, 0, co, 0, 0, 0};
    for (int i = 0; i < 3; i++)
        q[9 + i] = targets[1][i] - (q[i] * c[0] + q[3 + i] * c[1] + q[6 + i] * c[2]);
    compose(views[1], q, shown);
    return 1;
}
