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
 * over the hunter, +8 its distance and +0x10 the height it looks at. The game raises the eye over slopes and where the
 * scenery is in the way (to about 55 degrees, closer). Moving the mouse up and down raises the eye over the elevation
 * the game gives it, about the target at the same distance - in the frames shown (MHFU_FPS), at once and as far as the
 * yaw turns for the same motion, the game's camera left as it is (from -20 to 80 degrees). Without the mouse having
 * the camera (replaying a recording made before) it pitches the preset in use instead, which the game eases to (about
 * 0.2 degrees a frame when asked 0.5, settling short).
 *
 * The game also turns the camera itself where the scenery is in the way (it casts rays between the eye and the hunter);
 * with the mouse having the camera (rt_camera_free) those turns are patched out (recomp/patches.py): the camera stays
 * where the mouse puts it. MHFU_CAMERA_ASSIST=1 leaves them in.
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
 * Where the camera keeps its own angles (the village, the camp tent: view 2) the mouse orbits the picture about the
 * camera's target instead (with MHFU_FPS: it is the frames shown that turn), the stick turned to match so that
 * forward is forward as shown. The game knows nothing of it: the camera can go into walls, and what the game places
 * on the screen from its own camera (name tags) stays where it was.
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
#define STICK_SIGN 1.0f

int rt_camera_free = 1;
static float sens = -1;
static float left;                      /* yaw not yet written (in 65536ths) */
static unsigned long tick;              /* the game's frames, counted */
static int active;                      /* the camera is the hunt's, which the mouse turns */
/* Elsewhere (the village, the camp tent: view 2) the game's camera keeps its own angles; the mouse orbits the picture
 * about its target instead, by these (radians: yaw, and pitch up), and turns the stick to match */
static int orbiting, orbit_on = 1;
static float orbit_yaw, orbit_pitch;
/* In the hunt, with the mouse having the camera (rt_camera_free) and MHFU_FPS: how far the mouse has raised the eye
 * (radians) over the elevation the game gives it - which rises over slopes to keep it above the ground - shown at once
 * as an orbit about the target in the frames shown, the game's camera left as it is */
static float elevation;
static int elevation_set;
#define ELEVATION_LO (-20 * PI / 180)
#define ELEVATION_HI (80 * PI / 180)

static float clamp_elevation(float e) {
    return e < ELEVATION_LO ? ELEVATION_LO : e > ELEVATION_HI ? ELEVATION_HI : e;
}

/* The mouse's raise of the eye, kept so that the eye stays within the limits over the game's elevation */
static float clamp_raise(float raise, float game) {
    return clamp_elevation(game + raise) - game;
}

/* Off while replaying a recording made before the mouse orbited (version 1): its stick was not turned */
void camera_orbit_enable(int on) {
    orbit_on = on;
    if (!on)
        orbit_yaw = orbit_pitch = 0;
}

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
static float view_before[12];           /* the frame's before the previous (what some models were placed with) */
/* The cameras the frames were drawn with, as the game had them: views and view_before are the frames' as shown - the
 * same, but at the native frame rate where the game's camera moves only at 30ths (as it does where nothing is converted:
 * the village): then they are between its places at the last two 30ths, one 30th behind, as what ticks at 30 is drawn
 * (gpu.c) */
static float game_views[2][12], game_view_before[12];
static float target_before[3], eye_before[3];
/* gpu.c: the frames shown are drawn with the camera of the update before theirs - and so are a frame behind what the
 * camera follows: the target (and eye) followed are then the frame's before */
static int frames_late;

void camera_frames_late(int late) {
    frames_late = late;
}
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
    /* with the mouse having the camera the game turns it exactly as far as it is told (its own turns patched out): an
     * estimate would only add noise - stuck from before, or taken on a slow turn, it delays the turn and jumps */
    if (rt_camera_free)
        x->r = 1;
    else if (fabsf(x->applied) > 0.002f) {
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
/* At the native frame rate, with MHFU_FPS (a game frame: its camera - view 4x3, eye, target): if the camera moves only
 * at 30ths, where to show it - between its places at the last two 30ths by how far the frame is past the last, one 30th
 * behind (gpu.c's native smoothing: what ticks at 30 is drawn so). 0 if as it is */
int native_frame30(int back);
float rt_frame_30ths(void);
void gpu_camera_between(const float *a, const float *b, float t, float *out);
static int camera_between_30ths(const float *view, const float *e, const float *t, float *out, float *out_eye,
                                 float *out_target) {
    static float at30[2][12], eye30[2][3], target30[2][3], last[12];
    static int have30, moved, slow;
    if (rt_frame_30ths() > 0.999f) {
        have30 = 0;
        return 0;
    }
    int is30 = native_frame30(0);
    if (have30 && !is30 && memcmp(view, last, sizeof last))
        moved = 1;                      /* it moved between 30ths: the game moves it every frame */
    memcpy(last, view, sizeof last);
    if (is30) {
        memcpy(at30[0], at30[1], sizeof at30[0]);
        memcpy(at30[1], view, sizeof at30[1]);
        memcpy(eye30[0], eye30[1], sizeof eye30[0]);
        memcpy(eye30[1], e, sizeof eye30[1]);
        memcpy(target30[0], target30[1], sizeof target30[0]);
        memcpy(target30[1], t, sizeof target30[1]);
        have30 = have30 < 2 ? have30 + 1 : 2;
        slow = !moved;
        moved = 0;
    }
    if (have30 < 2 || !slow || !memcmp(at30[0], at30[1], sizeof at30[0]))
        return 0;
    float d = sqrtf((eye30[1][0] - eye30[0][0]) * (eye30[1][0] - eye30[0][0]) +
                    (eye30[1][1] - eye30[0][1]) * (eye30[1][1] - eye30[0][1]) +
                    (eye30[1][2] - eye30[0][2]) * (eye30[1][2] - eye30[0][2]));
    if (d > 600)                        /* not across a cut */
        return 0;
    int since = 0;
    while (since < 31 && !native_frame30(since))
        since++;
    float p = (float)since * rt_frame_30ths();
    if (p > 1)
        p = 1;
    gpu_camera_between(at30[0], at30[1], p, out);
    for (int i = 0; i < 3; i++) {
        out_eye[i] = eye30[0][i] + (eye30[1][i] - eye30[0][i]) * p;
        out_target[i] = target30[0][i] + (target30[1][i] - target30[0][i]) * p;
    }
    return 1;
}

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
    memcpy(game_view_before, game_views[0], sizeof game_view_before);
    memcpy(game_views[0], game_views[1], sizeof game_views[0]);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 3; i++)
            game_views[1][3 * j + i] = vm[4 * j + i];
    float between[12], between_eye[3], between_target[3];
    if (camera_between_30ths(game_views[1], e, t, between, between_eye, between_target))
        vm = NULL, e = between_eye, t = between_target;
    memcpy(view_before, views[0], sizeof view_before);
    memcpy(target_before, targets[0], sizeof target_before);
    memcpy(eye_before, eyes[0], sizeof eye_before);
    memcpy(views[0], views[1], sizeof views[0]);
    memcpy(targets[0], targets[1], sizeof targets[0]);
    memcpy(eyes[0], eyes[1], sizeof eyes[0]);
    if (vm)
        memcpy(views[1], game_views[1], sizeof views[1]);
    else
        memcpy(views[1], between, sizeof views[1]);
    memcpy(targets[1], t, sizeof targets[1]);
    memcpy(eyes[1], e, sizeof eyes[1]);
    if (have < 2)
        have++;
    float ox = e[0] - t[0], oy = e[1] - t[1], oz = e[2] - t[2];
    axis_frame(&yaw_axis, atan2f(ox, oz));
    axis_frame(&pitch_axis, atan2f(oy, sqrtf(ox * ox + oz * oz)));
    /* the hunt's camera turns with the yaw; the others keep their own angles, and the mouse orbits the picture */
    active = view_no == 0;
    orbiting = !active;
    if (active || !orbit_on)
        orbit_yaw = orbit_pitch = 0;
    else {
        /* MHFU_TEST_ORBIT=degrees[,frame]: orbit so far (from the game's frame), for testing */
        const char *test = getenv("MHFU_TEST_ORBIT");
        if (test && tick >= (strchr(test, ',') ? strtoul(strchr(test, ',') + 1, NULL, 10) : 0))
            orbit_yaw = (float)atof(test) * PI / 180;
        orbit_yaw = wrap(orbit_yaw - dx * sens * PI / 180);
        float el = pitch_axis.angle[1], p = orbit_pitch + dy * sens * PI / 180;
        float lo = -20 * PI / 180 - el, hi = 80 * PI / 180 - el;
        orbit_pitch = p < lo ? lo : p > hi ? hi : p;
    }
    /* moving the mouse right turns the camera right: the yaw goes down */
    left += -dx * sens * 65536.0f / 360.0f;
    int turn = (int)left;
    if (turn) {
        left -= (float)turn;
        yaw[0] = (uint16_t)(yaw[0] + turn);
        yaw[1] = (uint16_t)(yaw[1] + turn);
        yaw_axis.written = (float)turn * 2 * PI / 65536;
    }
    /* the pitch: moving the mouse up looks up, the eye going down around the hunter at the same distance - in the
     * frames shown (the eye's elevation, the game's camera left as it is), or, without them or replaying a recording
     * made before, by pitching the game's preset */
    if (active && rt_camera_free) {
        /* the game putting the camera behind the hunter (L: +0x12E while it turns there) puts the height back too */
        if (!elevation_set || *MEM(view + 0x12E))
            elevation = 0, elevation_set = 1;
        elevation = clamp_raise(elevation + dy * sens * PI / 180, pitch_axis.angle[1]);
        return;
    }
    elevation_set = 0;
    uint32_t preset = *(uint32_t *)MEM(view + 0x110);
    if (active && dy != 0 && in_ram(preset))
        pitch_axis.written = pitch(preset, dy * sens * PI / 180.0f);
}

/* The stick, turned with the picture where the mouse orbits it: forward is then forward as shown */
void camera_stick(uint8_t *lx, uint8_t *ly) {
    if (!orbiting || orbit_yaw == 0)
        return;
    float x = (float)*lx - 128, y = (float)*ly - 128, c = cosf(orbit_yaw), s = sinf(orbit_yaw);
    float rx = x * c + y * s * STICK_SIGN, ry = y * c - x * s * STICK_SIGN;
    rx += 128;
    ry += 128;
    *lx = (uint8_t)(rx < 0 ? 0 : rx > 255 ? 255 : rx + 0.5f);
    *ly = (uint8_t)(ry < 0 ? 0 : ry > 255 ? 255 : ry + 0.5f);
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
    if ((!active && !orbiting) || have < 2 || sens < 0)
        return 0;
    invert(game_views[0], prev_inv);
    invert(game_views[1], latest_inv);
    float moved = sqrtf((eyes[1][0] - eyes[0][0]) * (eyes[1][0] - eyes[0][0]) +
                        (eyes[1][1] - eyes[0][1]) * (eyes[1][1] - eyes[0][1]) +
                        (eyes[1][2] - eyes[0][2]) * (eyes[1][2] - eyes[0][2]));
    if (fabsf(wrap(yaw_axis.angle[1] - yaw_axis.angle[0])) > PI / 3 || moved > 600)
        interpolating = 0;
    float dx, dy;
    window_mouse_peek(&dx, &dy);
    float a, b;
    if (orbiting) {
        /* the game's camera as interpolated, orbited by the mouse's (at once) */
        float ty = wrap(yaw_axis.angle[1] - yaw_axis.angle[0]), tp = pitch_axis.angle[1] - pitch_axis.angle[0];
        a = (interpolating ? -(1 - t) * ty : 0) + orbit_yaw - dx * sens * PI / 180;
        b = (interpolating ? -(1 - t) * tp : 0) + orbit_pitch + dy * sens * PI / 180;
    } else {
        a = axis_shown(&yaw_axis, t, interpolating, (left - dx * sens * 65536.0f / 360.0f) * 2 * PI / 65536);
        if (elevation_set) {   /* the game's elevation at t, raised by the mouse's: up to it from the latest's */
            float game = interpolating ? pitch_axis.angle[0] + (pitch_axis.angle[1] - pitch_axis.angle[0]) * t
                                       : pitch_axis.angle[1];
            b = clamp_elevation(game + elevation + dy * sens * PI / 180.0f) - pitch_axis.angle[1];
        }
        else
            b = axis_shown(&pitch_axis, t, interpolating, dy * sens * PI / 180.0f);
    }
    /* MHFU_CAMERA_LOG=file: each frame shown, its time, t and the yaw and pitch shown (for checking smoothness) */
    static FILE *log;
    static int log_read;
    if (!log_read) {
        log_read = 1;
        if (getenv("MHFU_CAMERA_LOG"))
            log = fopen(getenv("MHFU_CAMERA_LOG"), "w");
    }
    const float *t0 = frames_late ? target_before : targets[0], *t1 = frames_late ? targets[0] : targets[1];
    const float *e0 = frames_late ? eye_before : eyes[0], *e1 = frames_late ? eyes[0] : eyes[1];
    float c[3];
    for (int i = 0; i < 3; i++)
        c[i] = interpolating ? t0[i] + (t1[i] - t0[i]) * t : t1[i];
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
    /* the eye's distance from the target as interpolated too (the game moves it in and out as the hunter goes): the
     * target, straight ahead, brought to that distance */
    if (interpolating) {
        float d0 = sqrtf((e0[0] - t0[0]) * (e0[0] - t0[0]) + (e0[1] - t0[1]) * (e0[1] - t0[1]) +
                         (e0[2] - t0[2]) * (e0[2] - t0[2]));
        float d1 = sqrtf((e1[0] - t1[0]) * (e1[0] - t1[0]) + (e1[1] - t1[1]) * (e1[1] - t1[1]) +
                         (e1[2] - t1[2]) * (e1[2] - t1[2]));
        float dl = sqrtf((eyes[1][0] - targets[1][0]) * (eyes[1][0] - targets[1][0]) +
                         (eyes[1][1] - targets[1][1]) * (eyes[1][1] - targets[1][1]) +
                         (eyes[1][2] - targets[1][2]) * (eyes[1][2] - targets[1][2]));
        if (dl > 1 && d1 > 1 && d0 > 1 && fabsf(d0 - dl) < 0.5f * dl) {
            float k = (d0 + (d1 - d0) * t) / dl, tv[3];
            for (int i = 0; i < 3; i++)
                tv[i] = shown[i] * c[0] + shown[3 + i] * c[1] + shown[6 + i] * c[2] + shown[9 + i];
            for (int i = 0; i < 3; i++)
                shown[9 + i] += tv[i] * (k - 1);
        }
    }
    if (log) {
        float ex[3];
        for (int i = 0; i < 3; i++)     /* the eye shown: -R^T t */
            ex[i] = -(shown[3 * i] * shown[9] + shown[3 * i + 1] * shown[10] + shown[3 * i + 2] * shown[11]);
        fprintf(log, "%llu %.4f %.6f %d %.6f %.6f %.1f %.2f %.2f %.2f %.2f %.2f %.2f\n", (unsigned long long)time_us(), t,
                yaw_axis.angle[1] + a, interpolating, pitch_axis.angle[1] + b, pitch_axis.angle[1],
                sqrtf((eyes[1][0] - targets[1][0]) * (eyes[1][0] - targets[1][0]) + (eyes[1][1] - targets[1][1]) *
                      (eyes[1][1] - targets[1][1]) + (eyes[1][2] - targets[1][2]) * (eyes[1][2] - targets[1][2])),
                c[0], c[1], c[2], ex[0], ex[1], ex[2]);
    }
    return 1;
}

/* gpu.c, at the native frame rate (MHFU_TICK_HZ above 30, no MHFU_FPS: each game frame drawn as the game draws it):
 * the camera to show the frame with - the game's now (view, 4x3 as gpu.c's; target, or NULL: the game's), turned as
 * camera_views turns the
 * latest, at once: where the game keeps its own angles the mouse's orbit, in the hunt the mouse's raise of the eye and
 * its turn not read yet. 0 if it is the game's as it is */
int camera_native_shown(const float *view, const float *target, float *shown) {
    uint32_t scene = *(uint32_t *)MEM(SCENE_PTR);
    if ((!active && !orbiting) || sens < 0 || !in_ram(scene))
        return 0;
    /* the eye: where the view puts it; the target: as given (the game's moved between 30ths too), or the game's */
    float e[3];
    for (int i = 0; i < 3; i++)
        e[i] = -(view[3 * i] * view[9] + view[3 * i + 1] * view[10] + view[3 * i + 2] * view[11]);
    const float *c = target ? target : (const float *)MEM(scene + 0xB50);
    float ox = e[0] - c[0], oy = e[1] - c[1], oz = e[2] - c[2];
    float yaw = atan2f(ox, oz), elev = atan2f(oy, sqrtf(ox * ox + oz * oz));
    float dx, dy;
    window_mouse_peek(&dx, &dy);
    float a, b;
    if (orbiting) {
        a = orbit_yaw - dx * sens * PI / 180;
        b = orbit_pitch + dy * sens * PI / 180;
    } else {
        a = (left - dx * sens * 65536.0f / 360.0f) * 2 * PI / 65536;
        b = elevation_set ? clamp_elevation(elev + elevation + dy * sens * PI / 180.0f) - elev : 0;
    }
    if (a == 0 && b == 0)
        return 0;
    const float up[3] = {0, 1, 0};
    float yaw_m[9], pitch_m[9], m[9];
    rotation(up, a, yaw_m);
    float psi = yaw + a;
    const float k[3] = {cosf(psi), 0, -sinf(psi)};
    rotation(k, -b, pitch_m);
    mul33(pitch_m, yaw_m, m);
    float q[12];
    for (int j = 0; j < 3; j++)                 /* M^T, about the target */
        for (int i = 0; i < 3; i++)
            q[3 * j + i] = m[3 * i + j];
    for (int i = 0; i < 3; i++)
        q[9 + i] = c[i] - (q[i] * c[0] + q[3 + i] * c[1] + q[6 + i] * c[2]);
    compose(view, q, shown);
    return 1;
}

/* For shadows (gpu.c): the game's camera now - the inverse of its view matrix (the world from what the game draws in:
 * it multiplies the camera into each world matrix) and the point it looks at. 0 if there is no scene */
int camera_game(float *inv, float *target) {
    uint32_t scene = *(uint32_t *)MEM(SCENE_PTR);
    if (!in_ram(scene) || *MEM(scene + 0xA8D) >= 6)
        return 0;
    const float *vm = (const float *)MEM(scene + 0xB80);
    float v[12];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 3; i++)
            v[3 * j + i] = vm[4 * j + i];
    if (!(fabsf(v[0]) + fabsf(v[4]) + fabsf(v[8]) > 0.5f))
        return 0;
    invert(v, inv);
    memcpy(target, MEM(scene + 0xB50), 3 * sizeof(float));
    return 1;
}

/* The inverse of a camera (4x3, as above) */
void camera_invert(const float *v, float *out) {
    invert(v, out);
}


/* gpu.c: the inverse of the camera two game frames ago. The game places some models (the monsters) during their update,
 * with the camera as it was before the frame's: a frame's drawing of them has the camera of the frame before in it */
void camera_before_inverse(float *inv) {
    invert(game_view_before, inv);
}

