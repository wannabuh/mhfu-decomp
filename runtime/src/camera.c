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
 * The other views (1, 3-5) are the game's event cameras: talking to someone, cutscenes. Views 3-5 take over what is
 * shown while the view in use stays the hunt's or the village's - the scene's byte at +0xDC4 says which (EBOOT
 * 08814E18; below 1: none). While one is shown the mouse leaves the camera alone: no turn, orbit or raise.
 *
 * Where the camera keeps its own angles (the village, the camp tent: view 2) the mouse orbits the picture about the
 * camera's target instead (with MHFU_FPS: it is the frames shown that turn), the stick turned to match so that
 * forward is forward as shown. The game knows nothing of it: the camera can go into walls, and what the game places
 * on the screen from its own camera (name tags) stays where it was.
 *
 * Free look (controls.c's free_look held, Left Alt): in the hunt the mouse turns the picture alone - the game's camera,
 * which the stick moves the hunter by, left where it is - so the hunter keeps going the same way while the camera
 * looks about; let go, the picture turns back (in a tenth of a second or so). The game is given none of the mouse's
 * motion meanwhile (recorded so). In the frames shown: with MHFU_FPS or at the native frame rate.
 *
 * MHFU_CAMERA_DISTANCE: how far the eye is from what it looks at, times the game's (default 1; e.g. 1.3 farther,
 * 0.8 closer) - in the frames shown, as the pitch is: the game's camera left as it is.
 *
 * The eye shown stays inside the area: raised, lowered, turned or moved back it isn't where the game's camera checked,
 * so each game frame it is cast for as the game's own camera does (ground and walls, between the target and the eye),
 * the game's memory and registers put back after, and the frames shown bring it in to what it hit - at once, easing
 * back out as the way clears.
 *
 * MHFU_MOUSE_SENS: degrees a mouse count turns (default 0.15). */
#include "hle.h"
#include "runtime.h"
#include "savestate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void window_mouse_peek(float *dx, float *dy);

#define SCENE_PTR 0x089CBD04u
#define PI 3.14159265f
#define STICK_SIGN 1.0f

SAVED int rt_camera_free = 1;
static float sens = -1;
SAVED static float left;                      /* yaw not yet written (in 65536ths) */
SAVED static unsigned long tick;              /* the game's frames, counted */
SAVED static int active;                      /* the camera is the hunt's, which the mouse turns */
/* Elsewhere (the village, the camp tent: view 2) the game's camera keeps its own angles; the mouse orbits the picture
 * about its target instead, by these (radians: yaw, and pitch up), and turns the stick to match */
SAVED static int orbiting, orbit_on = 1;
SAVED static float orbit_yaw, orbit_pitch;
/* In the hunt, with the mouse having the camera (rt_camera_free) and MHFU_FPS: how far the mouse has raised the eye
 * (radians) over the elevation the game gives it - which rises over slopes to keep it above the ground - shown at once
 * as an orbit about the target in the frames shown, the game's camera left as it is */
SAVED static float elevation;
SAVED static int elevation_set;
#define ELEVATION_LO (-20 * PI / 180)
#define ELEVATION_HI (80 * PI / 180)

/* Free look: how far the picture is turned from the game's camera (radians: yaw, and pitch up), not the game's */
static float look_yaw, look_pitch;
static int looking;

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
SAVED static Axis yaw_axis = {.r = 1, .before_r = 1}, pitch_axis = {.r = 1, .before_r = 1};

/* The game's last two frames' cameras: view matrices (4x3, as gpu.c's), targets, eyes; have: how many are recorded */
SAVED static float views[2][12], targets[2][3], eyes[2][3];
SAVED static float view_before[12];           /* the frame's before the previous (what some models were placed with) */
/* The cameras the frames were drawn with, as the game had them: views and view_before are the frames' as shown - the
 * same, but at the native frame rate where the game's camera moves only at 30ths (as it does where nothing is converted:
 * the village): then they are between its places at the last two 30ths, one 30th behind, as what ticks at 30 is drawn
 * (gpu.c) */
SAVED static float game_views[2][12], game_view_before[12];
SAVED static float target_before[3], eye_before[3];
/* gpu.c: the frames shown are drawn with the camera of the update before theirs - and so are a frame behind what the
 * camera follows: the target (and eye) followed are then the frame's before */
SAVED static int frames_late;

void camera_frames_late(int late) {
    frames_late = late;
}
SAVED static int have;

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
SAVED static struct { uint32_t addr; float radius, angle, height, distance; } pitched[16];

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
    SAVED static float at30[2][12], eye30[2][3], target30[2][3], last[12];
    SAVED static int have30, moved, slow;
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

/* -- staying inside the area ---------------------------------------------------------------------------------------- */

#define STAGE_MANAGER 0x09A4F1A0u       /* its byte +0xA3E8: the stage is loaded (stage_mgr_is_loaded) */
#define STAGE_LOADED 0xA3E8u
#define COLLISION_MANAGER 0x09D1A160u   /* what the casts are given (in the hunt's overlay: nothing else has it) */
#define CAST_GROUND 0x09C3DF10u         /* collision_segment_cast_grid_214: ground, all that is under the eye */
#define CAST_WALLS 0x09C3E3D0u          /* collision_segment_cast_grid_210_ignore_mask: walls */
#define WALLS_MASK 0x8009u              /* what the game's camera leaves out of them */
#define EYE_MARGIN 30.0f                /* how far from what it hits the eye stays (the game's scripts: 30 over ground) */
#define EYE_NEAREST 150.0f              /* never closer to the target than this (nearer, it is in the hunter) */
#define EYE_OWN_FROM (2 * PI / 180)     /* turned this little from the game's eye, the game's distance is as good as checked */
#define EYE_OWN_TO (12 * PI / 180)      /* (and from this much, not at all) */
#define EYE_NO_LIMIT 1e9f
#define EYE_EASE_OUT 900.0f             /* units a second it moves back out */
#define CAST_STACK 0x1000u              /* of the guest's stack, put back after (they use under 0x200) */

/* The most the eye shown may be from the target, this game frame's and the one's before (between them the frames shown
 * ease out; in, they take it at once). The picture's alone: not saved, made again in a frame */
static float eye_limit = EYE_NO_LIMIT, eye_limit_before = EYE_NO_LIMIT;

/* What the casts write: the manager's scratch (the cell range, the triangles' list), the guest's stack and registers */
#define MANAGER_SCRATCH_A 0x200u
#define MANAGER_SCRATCH_A_END 0x260u
#define MANAGER_SCRATCH_B 0x8C0u
#define MANAGER_SCRATCH_B_END 0x8E0u

/* How far along the way from target towards dir (a unit vector) the eye can be, up to want: casts the game's own way,
 * nothing of the game changed by it. EYE_NO_LIMIT without collision (the hunt's overlay not there, none yet) */
static float cast_hits[2];              /* the last casts' hits, from the target: ground, walls (-1: none) - for the log */

static float clear_distance(const float *target, const float *dir, float want) {
    cast_hits[0] = cast_hits[1] = -1;
    Ctx *c = thread_ctx();
    if (!c || rt_resume_pc)
        return EYE_NO_LIMIT;
    const RecompModule *m1, *m2;
    RecompFunc ground = rt_lookup(CAST_GROUND, &m1), walls = rt_lookup(CAST_WALLS, &m2);
    if (!ground || !walls || strcmp(m1->name, "game_sub") || strcmp(m2->name, "game_sub"))
        return EYE_NO_LIMIT;
    /* (changing area, the grids' pointers stay while their memory is reused: they count once the stage is loaded) */
    uint32_t stage = *(uint32_t *)MEM(STAGE_MANAGER);
    if (!in_ram(stage) || !*MEM(stage + STAGE_LOADED))
        return EYE_NO_LIMIT;
    uint32_t mgr = *(uint32_t *)MEM(COLLISION_MANAGER);
    uint32_t sp = c->r[29];
    if (!in_ram(mgr) || !in_ram(mgr + MANAGER_SCRATCH_B_END) || !in_ram(*(uint32_t *)MEM(mgr + 0x214)) ||
        !in_ram(*(uint32_t *)MEM(mgr + 0x210)) || !in_ram(sp - CAST_STACK) || !in_ram(sp))
        return EYE_NO_LIMIT;
    static Ctx saved;
    static uint8_t saved_stack[CAST_STACK], saved_a[MANAGER_SCRATCH_A_END - MANAGER_SCRATCH_A],
        saved_b[MANAGER_SCRATCH_B_END - MANAGER_SCRATCH_B];
    saved = *c;
    memcpy(saved_stack, MEM(sp - CAST_STACK), CAST_STACK);
    memcpy(saved_a, MEM(mgr + MANAGER_SCRATCH_A), sizeof saved_a);
    memcpy(saved_b, MEM(mgr + MANAGER_SCRATCH_B), sizeof saved_b);

    uint32_t from = (sp - 0x40) & ~15u, to = from + 16 - 0x40, hit = to - 16;
    float limit = EYE_NO_LIMIT;
    for (int walls_now = 0; walls_now < 2; walls_now++) {
        /* to the ground: an end lower than the eye, its share of the way kept (at a low angle, it stays over the ground
         * by about the margin); walls: an end beyond it */
        float end[4], reach = want + EYE_MARGIN;
        for (int i = 0; i < 3; i++)
            end[i] = target[i] + dir[i] * (walls_now ? reach : want);
        if (!walls_now)
            end[1] -= EYE_MARGIN;
        end[3] = 1;
        float start[4] = {target[0], target[1], target[2], 1};
        memcpy(MEM(from), start, 16);
        memcpy(MEM(to), end, 16);
        memcpy(MEM(hit), end, 16);
        c->r[4] = mgr;
        c->r[5] = from;
        c->r[6] = to;
        c->r[7] = hit;
        c->r[8] = WALLS_MASK;
        c->r[29] = (hit - 0x20) & ~15u;
        (walls_now ? walls : ground)(c);
        if (!(c->r[2] & 0xFF))
            continue;
        float at[3], len = 0, full = 0;
        memcpy(at, MEM(hit), sizeof at);
        for (int i = 0; i < 3; i++) {
            len += (at[i] - target[i]) * (at[i] - target[i]);
            full += (end[i] - target[i]) * (end[i] - target[i]);
        }
        len = sqrtf(len);
        full = sqrtf(full);
        /* (the wall cast's answer when an end is out of the grid: that end. The target out: no use) */
        if (len < 1 || !(len < full))
            continue;
        cast_hits[walls_now] = len;
        float allowed = walls_now ? len - EYE_MARGIN : want * len / full;
        if (allowed < limit)
            limit = allowed;
    }
    *c = saved;
    memcpy(MEM(sp - CAST_STACK), saved_stack, CAST_STACK);
    memcpy(MEM(mgr + MANAGER_SCRATCH_A), saved_a, sizeof saved_a);
    memcpy(MEM(mgr + MANAGER_SCRATCH_B), saved_b, sizeof saved_b);
    return limit < EYE_NEAREST ? EYE_NEAREST : limit;
}

static float distance_factor(void);
unsigned long present_frame_number(void);
static void view_distance(float *view, const float *c, float k);

/* This game frame's limit on the eye shown, for the way the eye is now going to be turned (the game's angles and the
 * mouse's: what comes between this game frame and the next stays within the margin). turned: how far that way is from
 * the game's own. The game has placed its eye: along its own way the limit takes in only what is beyond it */
static void eye_limit_update(float yaw, float elev, float turned) {
    eye_limit_before = eye_limit;
    float d = 0;
    for (int i = 0; i < 3; i++)
        d += (eyes[1][i] - targets[1][i]) * (eyes[1][i] - targets[1][i]);
    float own = sqrtf(d), want = own * distance_factor();
    float dir[3] = {cosf(elev) * sinf(yaw), sinf(elev), cosf(elev) * cosf(yaw)};
    float clear = want > EYE_NEAREST ? clear_distance(targets[1], dir, want) : EYE_NO_LIMIT;
    float away = (turned - EYE_OWN_FROM) / (EYE_OWN_TO - EYE_OWN_FROM);
    away = away < 0 ? 0 : away > 1 ? 1 : away;
    float least = EYE_NEAREST + (own - EYE_NEAREST) * (1 - away * away * (3 - 2 * away));
    float cast = clear;
    if (clear < least)
        clear = least;
    if (clear < eye_limit)
        eye_limit = clear;
    else {
        static float hz;                /* (the game's frames a second) */
        if (!hz)
            hz = getenv("MHFU_TICK_HZ") && atof(getenv("MHFU_TICK_HZ")) > 30 ? (float)atof(getenv("MHFU_TICK_HZ")) : 30;
        float step = EYE_EASE_OUT / hz;
        /* (no more to ease out to than what is wanted) */
        if (eye_limit > want && clear > want)
            eye_limit = clear;
        else
            eye_limit = eye_limit + step < clear ? eye_limit + step : clear;
    }
    /* MHFU_EYE_LOG=file: each game frame, the game's own eye and what the limit made of it (for #29) */
    static FILE *log;
    static int log_read;
    if (!log_read) {
        log_read = 1;
        if (getenv("MHFU_EYE_LOG") && (log = fopen(getenv("MHFU_EYE_LOG"), "w")))
            fprintf(log, "# frame target_x y z game_yaw game_elev own shown_yaw shown_elev turned want ground_hit wall_hit "
                         "cast least limit (angles in degrees; a hit -1: none; cast and limit 1e9: none)\n");
    }
    if (log) {
        float lim = eye_limit < want ? eye_limit : want;
        fprintf(log, "%lu %.1f %.1f %.1f %.2f %.2f %.1f %.2f %.2f %.2f %.1f %.1f %.1f %.1f %.1f %.1f\n", present_frame_number(),
                targets[1][0], targets[1][1], targets[1][2], yaw_axis.angle[1] * 180 / PI, pitch_axis.angle[1] * 180 / PI, own,
                yaw * 180 / PI, elev * 180 / PI, turned * 180 / PI, want, cast_hits[0], cast_hits[1],
                cast < want ? cast : want, least, lim);
        fflush(log);
    }
}

/* A view (4x3, as gpu.c's) looking at the point c brought within the limit, t of the way from the last game frame's */
static void eye_limit_apply(float *view, const float *c, float t) {
    float lim = eye_limit > eye_limit_before ? eye_limit_before + (eye_limit - eye_limit_before) * t : eye_limit;
    if (lim >= EYE_NO_LIMIT)
        return;
    float d = 0;
    for (int i = 0; i < 3; i++) {
        float tv = view[i] * c[0] + view[3 + i] * c[1] + view[6 + i] * c[2] + view[9 + i];
        d += tv * tv;
    }
    d = sqrtf(d);
    if (d > lim)
        view_distance(view, c, lim / d);
}

void camera_update(float dx, float dy) {
    camera_sens();
    tick++;
    active = 0;
    uint32_t scene = *(uint32_t *)MEM(SCENE_PTR);
    uint8_t view_no = in_ram(scene) ? *MEM(scene + 0xA8D) : 0xFF;
    eye_limit_before = eye_limit;
    if (view_no >= 6) {
        yaw_axis.written = pitch_axis.written = 0;
        eye_limit = eye_limit_before = EYE_NO_LIMIT;
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
    /* an event camera (talking to someone, a cutscene): the game's, as it is */
    if ((view_no != 0 && view_no != 2) || (int8_t)*MEM(scene + 0xDC4) > 0) {
        orbiting = 0;
        left = 0;
        yaw_axis.written = pitch_axis.written = 0;
        eye_limit = eye_limit_before = EYE_NO_LIMIT;
        return;
    }
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
    /* (an experiment for #29, MHFU_TEST_UNCATCH=1: the game's count at +0x12D kept above 0, as its own turns by the
     * D-pad set it - blocked, it then takes the eye to the wall on the way from the target instead of holding it) */
    if (active && rt_camera_free && getenv("MHFU_TEST_UNCATCH") && !*MEM(view + 0x12D))
        *MEM(view + 0x12D) = 1;
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
        float shown_elev = clamp_elevation(pitch_axis.angle[1] + elevation + look_pitch);
        eye_limit_update(yaw_axis.angle[1] + (float)turn * 2 * PI / 65536 + look_yaw, shown_elev,
                         fabsf(shown_elev - pitch_axis.angle[1]) + fabsf(look_yaw));
        return;
    }
    elevation_set = 0;
    uint32_t preset = *(uint32_t *)MEM(view + 0x110);
    if (active && dy != 0 && in_ram(preset))
        pitch_axis.written = pitch(preset, dy * sens * PI / 180.0f);
    if (active)
        eye_limit_update(yaw_axis.angle[1] + look_yaw, pitch_axis.angle[1] + look_pitch, fabsf(look_yaw) + fabsf(look_pitch));
    else
        eye_limit_update(yaw_axis.angle[1] + orbit_yaw, pitch_axis.angle[1] + orbit_pitch,
                         fabsf(orbit_yaw) + fabsf(orbit_pitch));
}

/* The eye's distance shown, times the game's (MHFU_CAMERA_DISTANCE) */
static float distance_factor(void) {
    static float k = -1;
    if (k < 0) {
        k = getenv("MHFU_CAMERA_DISTANCE") ? (float)atof(getenv("MHFU_CAMERA_DISTANCE")) : 1.0f;
        if (!(k >= 0.3f && k <= 3.0f))
            k = 1.0f;
    }
    return k;
}

/* A view (4x3, as gpu.c's) moved along the line to the point c it looks at, so that c is k times as far */
static void view_distance(float *view, const float *c, float k) {
    float tv[3];
    for (int i = 0; i < 3; i++)
        tv[i] = view[i] * c[0] + view[3 + i] * c[1] + view[6 + i] * c[2] + view[9 + i];
    for (int i = 0; i < 3; i++)
        view[9 + i] += tv[i] * (k - 1);
}

/* present.c, before camera_update (not replaying): free look held - the mouse's motion (dx, dy) the picture's alone,
 * taken from what the game is given; let go, the picture turned back towards the game's camera */
void camera_free_look(int held, float *dx, float *dy) {
    camera_sens();
    looking = held && active && rt_camera_free && sens > 0;
    if (looking) {
        look_yaw = wrap(look_yaw - *dx * sens * PI / 180);
        float p = look_pitch + *dy * sens * PI / 180;
        look_pitch = p < -PI / 2 ? -PI / 2 : p > PI / 2 ? PI / 2 : p;
        *dx = *dy = 0;
        return;
    }
    float k = expf(-rt_frame_30ths() / 30.0f / 0.04f);
    look_yaw = fabsf(look_yaw) < 1e-3f ? 0 : look_yaw * k;
    look_pitch = fabsf(look_pitch) < 1e-3f ? 0 : look_pitch * k;
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
/* The free camera, while the game is held (the C key): the picture seen from elsewhere, the mouse turning it and keys moving it, none of
 * the game's memory or its camera touched. What's in the picture is what the game drew, so what it left out as out of its
 * own view isn't there, and sprites keep the angle they were drawn at. */
static int free_on, free_hud_off;
static float free_eye[3], free_yaw, free_pitch;
static float last_shown[12];
static int have_shown;

int camera_free_on(void) { return free_on; }
int camera_free_hud_off(void) { return free_on && free_hud_off; }
void camera_note_shown(const float *v) {
    memcpy(last_shown, v, sizeof last_shown);
    have_shown = 1;
}

void camera_free_hud_toggle(void) {
    if (free_on)
        free_hud_off = !free_hud_off;
}

void camera_free_toggle(void) {
    if (free_on || have < 2) {
        free_on = 0;
        return;
    }
    float inv[12];
    invert(have_shown ? last_shown : game_views[1], inv);
    memcpy(free_eye, &inv[9], sizeof free_eye);
    free_yaw = atan2f(inv[6], inv[8]);
    free_pitch = asinf(inv[7] > 1 ? 1 : inv[7] < -1 ? -1 : inv[7]);
    free_on = 1;
    free_hud_off = 0;
}

void camera_free_end(void) {
    free_on = 0;
}

/* The mouse's counts, and how far to go along the camera's right, the world's up and the camera's front */
void camera_free_move(float dx, float dy, float right, float up, float front) {
    free_yaw -= dx * 0.15f * PI / 180;
    free_pitch += dy * 0.15f * PI / 180;
    free_pitch = free_pitch > 1.55f ? 1.55f : free_pitch < -1.55f ? -1.55f : free_pitch;
    float cp = cosf(free_pitch), back[3] = {cp * sinf(free_yaw), sinf(free_pitch), cp * cosf(free_yaw)};
    float rx = cosf(free_yaw), rz = -sinf(free_yaw);
    free_eye[0] += rx * right - back[0] * front;
    free_eye[1] += up - back[1] * front;
    free_eye[2] += rz * right - back[2] * front;
}

static void free_view(float *shown) {
    float cp = cosf(free_pitch), sp = sinf(free_pitch), sy = sinf(free_yaw), cy = cosf(free_yaw);
    float inv[12] = {cy, 0, -sy,  -sp * sy, cp, -sp * cy,  cp * sy, sp, cp * cy,  free_eye[0], free_eye[1], free_eye[2]};
    invert(inv, shown);
}

int camera_views(float t, int interpolating, float *prev_inv, float *latest_inv, float *shown) {
    if (free_on && have >= 2) {
        invert(game_views[0], prev_inv);
        invert(game_views[1], latest_inv);
        free_view(shown);
        return 1;
    }
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
        /* (free look: the mouse's motion not read yet the picture's, as the rest of its turn) */
        float game_dx = looking ? 0 : dx, game_dy = looking ? 0 : dy;
        float look_a = look_yaw - (looking ? dx * sens * PI / 180 : 0), look_b = look_pitch + (looking ? dy * sens * PI / 180 : 0);
        a = axis_shown(&yaw_axis, t, interpolating, (left - game_dx * sens * 65536.0f / 360.0f) * 2 * PI / 65536) + look_a;
        if (elevation_set) {   /* the game's elevation at t, raised by the mouse's: up to it from the latest's */
            float game = interpolating ? pitch_axis.angle[0] + (pitch_axis.angle[1] - pitch_axis.angle[0]) * t
                                       : pitch_axis.angle[1];
            b = clamp_elevation(game + elevation + look_b + game_dy * sens * PI / 180.0f) - pitch_axis.angle[1];
        }
        else
            b = axis_shown(&pitch_axis, t, interpolating, game_dy * sens * PI / 180.0f) + look_b;
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
    if (distance_factor() != 1.0f)
        view_distance(shown, c, distance_factor());
    eye_limit_apply(shown, c, interpolating ? t : 1);
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
        float game_dx = looking ? 0 : dx;
        float look_a = look_yaw - (looking ? dx * sens * PI / 180 : 0), look_b = look_pitch + (looking ? dy * sens * PI / 180 : 0);
        a = (left - game_dx * sens * 65536.0f / 360.0f) * 2 * PI / 65536 + look_a;
        b = elevation_set ? clamp_elevation(elev + elevation + look_b + (looking ? 0 : dy) * sens * PI / 180.0f) - elev
                          : look_b;
    }
    if (a == 0 && b == 0 && distance_factor() == 1.0f && sqrtf(ox * ox + oy * oy + oz * oz) <= eye_limit)
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
    if (distance_factor() != 1.0f)
        view_distance(shown, c, distance_factor());
    eye_limit_apply(shown, c, 1);
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

