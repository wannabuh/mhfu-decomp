/* Presenting frames: in the window (window.c), and written out as PNGs (MHFU_DUMP=N: every Nth frame, to
 * build/frames/), to compare with PPSSPP's.
 *
 * MHFU_FPS=N (with the Vulkan renderer) shows N frames a second, whatever the game's 30: the game's frames are
 * recorded (gpu.c), and the display draws its own from them - interpolated between the last two - in the scheduler's
 * idle time (present_pump). */
#define _GNU_SOURCE 1
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include "hle.h"
#include "savestate.h"
#include "render.h"
#include "perf.h"
#include "pacing.h"
#include "capture.h"
#include "inspect.h"

#include <png.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int dump_every = -1;
SAVED static unsigned long frame;
static int camera_orbit_ready;
static int display_fps(void);
void tick_frame(unsigned long frame);

unsigned long present_frame_number(void) {
    return frame;
}

/* MHFU_DEBUG_FRAME=N: called at the game's frame N, for a debugger to stop on (tools/watch.sh) */
__attribute__((noinline)) void debug_frame_hook(unsigned long n) {
    __asm__ volatile("" ::"r"(n) : "memory");
}

/* Scripted input, for running unattended: MHFU_INPUT="first-last:buttons,..." holds the buttons (PSP button bits, in
 * hex: 0x4000 cross, 0x2000 circle, 0x8 start, 0x10 up, 0x40 down...) from frame first to frame last; and the analog
 * stick, pushed fully: 0x10000 up, 0x20000 down, 0x40000 left, 0x80000 right */
extern uint32_t ctrl_buttons;
float camera_sens(void);               /* camera.c: the mouse's sensitivity, kept in recordings */
void camera_set_sens(float s);
void camera_orbit_enable(int on);
extern uint8_t ctrl_lx, ctrl_ly;

/* Skipping the intros (unless MHFU_INTRO=1, or a script drives the input - its frames count from a full boot): the
 * language menu is answered (cross: English, the first), then the game runs as fast as it can - its clock jumping to
 * whatever it waits for, nothing shown or heard - through the logos, to the title. The menu runs at its own pace,
 * for its second and a half. */
#define LANGUAGE_FRAME 40           /* the language menu takes cross (from about frame 35) */
#define TITLE_FRAME 470             /* the title, about 435 frames after the menu is answered */

static int skip_intro(void) {
    static int skip = -1;
    if (skip < 0)
        skip = !getenv("MHFU_INTRO") && !getenv("MHFU_INPUT");
    return skip;
}

/* Recording and replaying the input (MHFU_RECORD=file, MHFU_REPLAY=file): the game's clock being virtual (thread.c), a
 * run is the same for the same input, so replaying a recording - as fast as it goes, nothing shown or heard - brings
 * the game back to where it ended, and play carries on from there (recorded too, with MHFU_RECORD: a recording can be
 * extended). A file: "mhfu-replay 1", "epoch <us>" (the date and time the run started), "sens <degrees>" (the mouse's
 * sensitivity, MHFU_MOUSE_SENS, as the mouse turns the camera in the game), "camera <frame>" (the frame from which the
 * mouse had the camera: the game's own turns of it left out - none in recordings made before), then a line for each frame whose
 * input differs from the one before or moves the mouse: "<frame> <buttons> <stick x> <stick y> <mouse x> <mouse y>",
 * "pointer <frame> <x> <y> <clicks>" for the mouse moved (or clicked) in a menu (menu.c: what it moves is not all in
 * the buttons), and "end <frame>": the input as the game had it (the stick as turned by the mouse's orbit, camera.c). A save changed
 * since, or a runtime that runs the game differently, can make a replay go astray.
 * Test scenarios add "poke <frame>[-<last>] <where> <b|h|w|f> <value>": the game's memory written as the frame starts
 * (each frame to the last), where an address (hex) or "hunter+<hex>" (the hunt's hunter, if there is one) - a byte,
 * half, word (value in hex or decimal) or float (tools/suite.py). A recording made while replaying one carries its
 * pokes over. */
typedef struct { unsigned long frame; uint32_t buttons; int lx, ly; float mx, my; } Input;
static Input *replay;
static size_t replay_n;
SAVED static size_t replay_at;
static unsigned long replay_end;
static int replay_loaded;
static uint64_t replay_epoch_us;
static unsigned long replay_free_from = ~0ul;   /* "camera <frame>": the mouse has the camera from that frame */
typedef struct { unsigned long frame; float x, y; int clicks; } Pointer;
static Pointer *replay_pointer;
static size_t replay_pointer_n;
SAVED static size_t replay_pointer_at;
typedef struct { unsigned long first, last; uint32_t addr; int hunter; char type; uint32_t value; } Poke;
static Poke *replay_poke;
static size_t replay_poke_n;

static void parse_poke(const char *line) {
    Poke k = {0};
    char where[64], type;
    char value[32];
    if (sscanf(line, "%lu-%lu %63s %c %31s", &k.first, &k.last, where, &type, value) != 5) {
        if (sscanf(line, "%lu %63s %c %31s", &k.first, where, &type, value) != 4) {
            fprintf(stderr, "a poke line not understood: poke %s", line);
            return;
        }
        k.last = k.first;
    }
    k.hunter = strncmp(where, "hunter+", 7) == 0;
    k.addr = (uint32_t)strtoul(k.hunter ? where + 7 : where, NULL, 16);
    k.type = type;
    if (type == 'f') {
        float v = strtof(value, NULL);
        memcpy(&k.value, &v, 4);
    } else
        k.value = (uint32_t)strtoul(value, NULL, 0);
    replay_poke = realloc(replay_poke, (replay_poke_n + 1) * sizeof *replay_poke);
    replay_poke[replay_poke_n++] = k;
}

static void write_poke(FILE *f, const Poke *k) {
    char where[32];
    if (k->hunter)
        snprintf(where, sizeof where, "hunter+%X", k->addr);
    else
        snprintf(where, sizeof where, "%08X", k->addr);
    if (k->type == 'f') {
        float v;
        memcpy(&v, &k->value, 4);
        fprintf(f, "poke %lu-%lu %s f %.9g\n", k->first, k->last, where, v);
    } else
        fprintf(f, "poke %lu-%lu %s %c 0x%X\n", k->first, k->last, where, k->type, k->value);
}

extern uint32_t rt_local_player;

/* The runtime's own write of the hunter's memory, this frame (controls.c: the item bar's choice opened at once), as a
 * poke: written as the frame's pokes are, and recorded - a replay writes it again */
static FILE *record;
void present_poke_hunter(uint32_t offset, char type, uint32_t value) {
    Poke k = {frame, frame, offset, 1, type, value};
    replay_poke = realloc(replay_poke, (replay_poke_n + 1) * sizeof *replay_poke);
    replay_poke[replay_poke_n++] = k;
    if (record)
        write_poke(record, &k);
}

/* This frame's pokes (a scenario's: the game's memory written) */
static void replay_pokes(void) {
    for (size_t i = 0; i < replay_poke_n; i++) {
        Poke *k = &replay_poke[i];
        if (frame < k->first || frame > k->last)
            continue;
        uint32_t a = k->addr;
        if (k->hunter) {
            uint32_t h = rt_local_player;
            if (!h || LW(h) != 0x089BB3CCu)     /* (no hunt's hunter now) */
                continue;
            a += h;
        }
        switch (k->type) {
        case 'b': SB(a, k->value); break;
        case 'h': SH(a, k->value); break;
        default: SW(a, k->value); break;
        }
    }
}

static void load_replay(void) {
    if (replay_loaded)
        return;
    replay_loaded = 1;
    const char *path = getenv("MHFU_REPLAY");
    if (!path)
        return;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot read the replay %s\n", path);
        return;
    }
    char line[256];
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        Input in;
        unsigned long long e;
        float sens;
        if (sscanf(line, "epoch %llu", &e) == 1)
            replay_epoch_us = e;
        else if (sscanf(line, "sens %g", &sens) == 1)
            camera_set_sens(sens);
        else if (sscanf(line, "end %lu", &replay_end) == 1)
            ;
        else if (sscanf(line, "camera %lu", &replay_free_from) == 1)
            ;
        else if (strncmp(line, "poke ", 5) == 0)
            parse_poke(line + 5);
        else if (strncmp(line, "pointer ", 8) == 0) {
            Pointer p;
            if (sscanf(line + 8, "%lu %g %g %d", &p.frame, &p.x, &p.y, &p.clicks) == 4) {
                replay_pointer = realloc(replay_pointer, (replay_pointer_n + 1) * sizeof *replay_pointer);
                replay_pointer[replay_pointer_n++] = p;
            }
        }
        else if (sscanf(line, "%lu %x %d %d %g %g", &in.frame, &in.buttons, &in.lx, &in.ly, &in.mx, &in.my) == 6) {
            if (replay_n == cap) {
                cap = cap ? cap * 2 : 1024;
                replay = realloc(replay, cap * sizeof *replay);
            }
            replay[replay_n++] = in;
        }
    }
    fclose(f);
    if (!replay_end && replay_n)
        replay_end = replay[replay_n - 1].frame;
    fprintf(stderr, "replaying %s: %lu frames\n", path, replay_end);
}

/* From which game frame the mouse has the camera, the game's own turns of it left out (rt_camera_free, camera.c): from
 * the start - or, replaying a recording made before (no "camera" line), from where it hands over */
static unsigned long camera_free_from(void) {
    load_replay();
    if (getenv("MHFU_CAMERA_ASSIST") && atoi(getenv("MHFU_CAMERA_ASSIST")))
        return ~0ul;            /* MHFU_CAMERA_ASSIST=1: the game turns its camera as it would */
    if (replay_free_from != ~0ul)
        return replay_free_from;
    return replay_end;
}

/* thread.c: the date and time the replayed run started (0 if none) */
uint64_t replay_epoch(void) {
    load_replay();
    return replay_epoch_us;
}

static int replaying(void) {
    load_replay();
    return replay_end && frame < replay_end;
}

/* MHFU_REPLAY_SPEED=N: from the game's frame N on, a replay runs at the game's speed (to measure it, or to watch) */
static unsigned long replay_speed_at(void) {
    static long at = -1;
    if (at < 0)
        at = getenv("MHFU_REPLAY_SPEED") ? atol(getenv("MHFU_REPLAY_SPEED")) : 0;
    return at ? (unsigned long)at : ~0ul;
}

double rt_game_30ths(void);

/* The intros' frames are 30ths of the game's time (its frames at 30 a second: at MHFU_TICK_HZ, more of them) */
static double intro_time(void) {
    return rt_game_30ths();
}

int fast_forward(void) {
    return (skip_intro() && intro_time() >= LANGUAGE_FRAME + 10 && intro_time() < TITLE_FRAME) ||
           (replaying() && frame < replay_speed_at());
}

/* This frame's input, from the replay while there is one */
static void replay_input(float *mx, float *my) {
    if (!replaying())
        return;
    SAVED static Input held = {0, 0, 128, 128, 0, 0};
    *mx = *my = 0;
    while (replay_at < replay_n && replay[replay_at].frame <= frame) {
        held = replay[replay_at];
        if (held.frame == frame) {      /* (a frame's buttons can come again, as the game read them: present_input_read) */
            *mx += held.mx;
            *my += held.my;
        }
        replay_at++;
    }
    ctrl_buttons = held.buttons;
    ctrl_lx = (uint8_t)held.lx;
    ctrl_ly = (uint8_t)held.ly;
    if (frame + 1 == replay_end)
        fprintf(stderr, "replay done: over to you\n");
}

static unsigned long recorded_frame;

static void end_record(void) {
    if (record) {
        fprintf(record, "end %lu\n", recorded_frame);
        fclose(record);
        record = NULL;
    }
}

uint64_t run_epoch_us(void);

static void record_input(float mx, float my) {
    capture_note_input(frame, ctrl_buttons, ctrl_lx, ctrl_ly, mx, my);
    static int opened;
    static uint32_t last_b = ~0u;
    static int last_x = -1, last_y = -1;
    if (!opened) {
        opened = 1;
        const char *path = getenv("MHFU_RECORD"), *from = getenv("MHFU_REPLAY");
        if (path) {
            /* a recording is not overwritten - unless it is the one being replayed, which this run extends: another
             * name is taken instead (file.1, file.2...) */
            char name[4096];
            snprintf(name, sizeof name, "%s", path);
            for (int n = 1; access(name, F_OK) == 0 && !(from && strcmp(from, path) == 0); n++)
                snprintf(name, sizeof name, "%s.%d", path, n);
            if (strcmp(name, path) != 0)
                fprintf(stderr, "%s exists: recording to %s\n", path, name);
            record = fopen(name, "w");
            if (!record)
                fprintf(stderr, "cannot write the recording %s\n", name);
            else {
                fprintf(stderr, "recording to %s\n", name);
                fprintf(record, "mhfu-replay 1\nepoch %llu\nsens %.9g\ncamera %lu\n", (unsigned long long)run_epoch_us(),
                        camera_sens(), camera_free_from());
                for (size_t i = 0; i < replay_poke_n; i++)     /* (the replay's pokes: a scenario extended keeps them) */
                    write_poke(record, &replay_poke[i]);
                atexit(end_record);
            }
        }
    }
    if (!record)
        return;
    recorded_frame = frame;
    if (ctrl_buttons != last_b || ctrl_lx != last_x || ctrl_ly != last_y || mx != 0 || my != 0) {
        fprintf(record, "%lu %x %d %d %.9g %.9g\n", frame, ctrl_buttons, ctrl_lx, ctrl_ly, mx, my);
        fflush(record);
        last_b = ctrl_buttons;
        last_x = ctrl_lx;
        last_y = ctrl_ly;
    }
}

static uint64_t next_display;
static double next_display_fraction;

/* A state loaded (savestate.c): a recording of this run's input ends - its frames would go back -, and the frames
 * shown are due again from now (by the game's clock, which went back with it: else none until it got to where it was) */
void present_state_loaded(void) {
    next_display = 0;
    if (record) {
        fprintf(stderr, "recording stopped: a save state was loaded\n");
        end_record();
    }
}

/* The buttons held: the devices' (window_poll) and the script's */
static void scripted_input(uint32_t device_buttons) {
    static const char *script;
    static int read;
    if (!read) {
        script = getenv("MHFU_INPUT");
        read = 1;
    }
    ctrl_buttons = device_buttons;
    if (!script)
        return;
    uint32_t buttons = device_buttons;
    for (const char *p = script; *p;) {
        unsigned long first, last;
        unsigned int b;
        int n = 0;
        if (sscanf(p, "%lu-%lu:%x%n", &first, &last, &b, &n) != 3)
            break;
        if (frame >= first && frame <= last)
            buttons |= b;
        p += n;
        if (*p == ',')
            p++;
    }
    if (buttons & 0xF0000) {
        ctrl_ly = (buttons & 0x10000) ? 0 : (buttons & 0x20000) ? 255 : 128;
        ctrl_lx = (buttons & 0x40000) ? 0 : (buttons & 0x80000) ? 255 : 128;
    }
    ctrl_buttons = buttons & 0xFFFF;
}

static uint32_t pixel(uint32_t addr, int fmt) {
    if (fmt == 3)
        return LW(addr) | 0xFF000000u;
    uint32_t c = LHU(addr), r, g, b;
    switch (fmt) {
    case 0: r = c & 31; g = (c >> 5) & 63; b = (c >> 11) & 31; r = r << 3 | r >> 2; g = g << 2 | g >> 4; b = b << 3 | b >> 2; break;
    case 1: r = c & 31; g = (c >> 5) & 31; b = (c >> 10) & 31; r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2; break;
    default: r = (c & 15) * 17; g = ((c >> 4) & 15) * 17; b = ((c >> 8) & 15) * 17; break;
    }
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

int window_poll(void);
uint32_t window_buttons(void);
void window_stick(uint8_t *lx, uint8_t *ly);
void window_mouse(float *dx, float *dy);
void camera_update(float dx, float dy);
void camera_stick(uint8_t *lx, uint8_t *ly);
void camera_free_look(int held, float *dx, float *dy);
int controls_free_look(void);
void window_show(const uint32_t *frame);
uint32_t *window_pixels(void);

/* The displayed framebuffer as ABGR8888, 480 x 272 */
static void convert(uint32_t *out) {
    int bpp = display_fb_format == 3 ? 4 : 2;
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            out[y * 480 + x] = pixel(display_fb_addr + (uint32_t)((y * (int)display_fb_width + x) * bpp),
                                     (int)display_fb_format);
}

static void write_png_size(const char *path, const uint32_t *frame, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    png_init_io(png, f);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < h; y++)
        png_write_row(png, (png_bytep)(frame + (size_t)y * (size_t)w));
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
}

void present_write_png(const char *path, const uint32_t *pixels, int w, int h) {
    write_png_size(path, pixels, w, h);
}

static void write_png(const char *path, const uint32_t *frame) {
    write_png_size(path, frame, 480, 272);
}

/* A frame shown (timed for the performance panel, and noted: its rings) */
static void flip(uint32_t addr) {
    if (perf_on) {
        uint64_t t = perf_ns(), waited = perf_sync_ns;
        render_flip(addr);
        perf_flip_ns += perf_ns() - t - (perf_sync_ns - waited);
    } else
        render_flip(addr);
    perf_shown();
}

uint32_t *render_readback_full(uint32_t addr, int *w, int *h);
uint32_t *render_readback_present(uint32_t addr, int w, int h);

/* MHFU_WATCH="addr,addr,...": log each watched word (hex addresses) when it changes, checked every frame */
static void watch(void) {
    static uint32_t addrs[16], last[16];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("MHFU_WATCH");
        while (e && *e && n < 16) {
            char *end;
            addrs[n] = (uint32_t)strtoul(e, &end, 16);
            last[n] = LW(addrs[n]);
            n++;
            e = *end ? end + 1 : end;
        }
    }
    for (int i = 0; i < n; i++) {
        uint32_t v = LW(addrs[i]);
        if (v != last[i]) {
            fprintf(stderr, "watch frame %lu: [%08X] %08X -> %08X\n", frame, addrs[i], last[i], v);
            last[i] = v;
        }
    }
}

uint64_t wall_time_us(void), time_slept_us(void), game_to_wall_us(uint64_t game_us), wall_to_game_us(uint64_t wall);
int64_t clock_behind_us(void);
static unsigned long skipped;        /* frames not shown, the game running late (for MHFU_FPS_STATS) */
static uint64_t longest_frame_us;    /* the longest a game frame took, on the wall clock (for MHFU_FPS_STATS) */
static int fps = -1;                /* the display's frame rate; 0: the game's own frames */

static int display_fps(void) {
    if (fps < 0) {
        const char *e = getenv("MHFU_FPS");
        fps = e && render_on ? atoi(e) : 0;
        if (fps < 0 || fps > 1000)
            fps = 0;
        /* the game at as many frames a second itself (MHFU_TICK_HZ, from the start): none between to interpolate */
        const char *hz = getenv("MHFU_TICK_HZ");
        if (fps && hz && !strchr(hz, '@') && atoi(hz) >= fps) {
            fprintf(stderr, "display: the game runs at %d frames a second itself - MHFU_FPS's interpolation off\n",
                    atoi(hz));
            fps = 0;
        }
        if (fps) {
            gpu_record(1);
            fprintf(stderr, "display: %d frames a second, interpolated\n", fps);
        }
    }
    return fps;
}

/* Draw and show a frame if one is due; returns when the next is (or ~0 with no frame rate of its own) */
uint64_t present_pump(void) {
    if (!display_fps() || fast_forward())
        return ~(uint64_t)0;
    uint64_t now = time_us();
    /* when the next frame begins: for its refresh, if the display's are known (pacing.c); else on a timer at MHFU_FPS */
    uint64_t due_wall = 0, target_wall = 0, shows_wall = 0, wall_now = wall_time_us();
    int locked = pacing_next(fps, wall_now, &due_wall, &target_wall, &shows_wall);
    uint64_t interval = 1000000u / (uint64_t)fps, shows = now;
    if (locked) {
        next_display = wall_to_game_us(due_wall);
        shows = wall_to_game_us(shows_wall);
    } else if (!pacing_old() && next_display_fraction >= 1) {   /* (the whole frame: 1000000 / 144 is not a whole us) */
        next_display++;
        next_display_fraction -= 1;
    }
    if (now < next_display)
        return next_display;
    /* the game first: when it is running late (the frames shown taking the time it needs), a frame is not shown */
    if (clock_behind_us() > (int64_t)interval) {
        skipped++;
        perf_skipped++;
        if (locked)
            pacing_issued(fps, wall_now);
        else
            next_display = now + interval;
        return next_display;
    }
    /* the input first, so that the frame shows the mouse as it is now (as the game's frames do) */
    if (!window_poll())
        exit(0);
    /* the moment shown trails the game by one frame: t runs from 0, as a frame lands, towards 1 */
    int64_t since = (int64_t)shows - (int64_t)gpu_last_frame_time();
    uint64_t period = gpu_frame_period();
    float t = period ? (float)since / (float)period : 1.0f;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    uint64_t drawing = wall_time_us();
    if (pacing_watch)
        pacing_frame(game_to_wall_us(next_display), target_wall, drawing,
                     (int64_t)gpu_last_frame_time() - (int64_t)((1.0f - t) * (float)period), frame, t);
    if (locked)
        pacing_issued(fps, wall_now);
    uint32_t addr = gpu_render_frame(t);
    if (pacing_watch)
        pacing_drawn(wall_time_us());
    static long dump_at = -1;
    if (dump_at < 0)
        dump_at = getenv("MHFU_DUMP_AT") ? atol(getenv("MHFU_DUMP_AT")) : 0;
    /* MHFU_DUMP_AT=N: at the game's frame N, the frames between it and the one before at t = 0, 1/4 ... 1, for checking
     * the interpolation */
    static int dumped;
    if (addr && dump_at && (long)frame >= dump_at && !dumped) {
        dumped = 1;
        mkdir("build/frames", 0755);
        for (int k = 0; k <= 4; k++) {
            char path[64];
            gpu_render_frame((float)k / 4);
            render_readback(addr, window_pixels());
            snprintf(path, sizeof path, "build/frames/g%06lu-t%03d.png", frame, k * 25);
            write_png(path, window_pixels());
            if (k == 4 && getenv("MHFU_DUMP_FULL")) {       /* and the last at the internal resolution */
                int w, h;
                uint32_t *full = render_readback_full(addr, &w, &h);
                if (full) {
                    snprintf(path, sizeof path, "build/frames/g%06lu-full.png", frame);
                    write_png_size(path, full, w, h);
                    free(full);
                }
            }
            int pw, ph;                                     /* and as the window shows it at that size */
            if (k == 4 && getenv("MHFU_DUMP_PRESENT") && sscanf(getenv("MHFU_DUMP_PRESENT"), "%dx%d", &pw, &ph) == 2) {
                uint32_t *shown = render_readback_present(addr, pw, ph);
                if (shown) {
                    snprintf(path, sizeof path, "build/frames/g%06lu-present.png", frame);
                    write_png_size(path, shown, pw, ph);
                    free(shown);
                }
            }
        }
        gpu_render_frame(t);
    }
    uint64_t flipping = wall_time_us();
    if (perf_on)
        perf_draw_ns += (flipping - drawing) * 1000;
    if (addr && capture_wants_shot())
        capture_take_shot(addr, NULL);
    if (addr)
        flip(addr);
    static uint64_t flip_us;
    flip_us += wall_time_us() - flipping;
    static uint64_t stats_at, drawn_us;
    static unsigned long shown, stats_frame;
    shown++;
    drawn_us += wall_time_us() - drawing;
    if (getenv("MHFU_FPS_STATS")) {
        uint64_t wall = wall_time_us();
        if (!stats_at)
            stats_at = wall;
        if (wall - stats_at > 5000000) {
            unsigned long m, u;
            gpu_match_stats(&m, &u);
            double secs = (double)(wall - stats_at) / 1e6;
            fprintf(stderr, "display: %.1f frames a second (%lu left out, the game late), the game %.1f (longest frame "
                    "%.1f ms), idle %.0f%%, %.0f%% of primitives matched, a frame shown %.2f ms (%.2f flipping)\n",
                    (double)shown / secs, skipped, (double)(frame >= stats_frame ? frame - stats_frame : 0) / secs,
                    (double)longest_frame_us / 1000, (double)time_slept_us() / 1e4 / secs,
                    m + u ? 100.0 * (double)m / (double)(m + u) : 0.0, (double)drawn_us / 1000 / (double)(shown ? shown : 1),
                    (double)flip_us / 1000 / (double)(shown ? shown : 1));
            drawn_us = flip_us = 0;
            skipped = 0;
            stats_at = wall;
            stats_frame = frame;
            longest_frame_us = 0;
            shown = 0;
        }
    }
    if (locked)
        return now + 1;     /* (pacing_next says when: asked again at once) */
    next_display += interval;
    next_display_fraction += 1000000.0 / fps - (double)interval;
    if (next_display < now)
        next_display = now + interval;
    return next_display;
}

/* MHFU_DUMP with MHFU_DUMP_FULL: the frame at the internal resolution too */
static void dump_full(uint32_t addr, unsigned long at) {
    int w, h;
    uint32_t *full = getenv("MHFU_DUMP_FULL") ? render_readback_full(addr, &w, &h) : NULL;
    if (full) {
        char path[64];
        snprintf(path, sizeof path, "build/frames/%06lu-full.png", at);
        write_png_size(path, full, w, h);
        free(full);
    }
}

/* MHFU_SNAPSHOT="frame,frame,...": RAM at those game frames, to build/ram-<frame>.bin (finding variables by diffing) */
static void snapshots(void) {
    static const char *list;
    static int read;
    if (!read) {
        list = getenv("MHFU_SNAPSHOT");
        read = 1;
    }
    for (const char *p = list; p && *p;) {
        char *end;
        unsigned long f = strtoul(p, &end, 10);
        if (f == frame) {
            char path[64];
            snprintf(path, sizeof path, "build/ram-%06lu.bin", frame);
            FILE *out = fopen(path, "wb");
            if (out) {
                fwrite(MEM(0x08800000u), 1, 0x01800000u, out);
                fclose(out);
            }
        }
        p = *end ? end + 1 : end;
    }
}

void gpu_frame_tick(void);
void gpu_native_stats(unsigned long *smoothed, unsigned long *drawn);
int gpu_native_smoothing(void);
extern int gpu_skip_draw, gpu_divisor;
double rt_game_30ths(void);
float rt_frame_30ths(void);
static unsigned long native_skipped;    /* (for MHFU_FPS_STATS) */
void menu_input(uint32_t *buttons);

SAVED static int input_due;               /* the game has yet to read the input of this frame */

int window_pointer(float *x, float *y);
int window_pointer_moved(void);
int window_pointer_clicks(void);
int menu_test_pointer(float *x, float *y, int *moved, int *clicks);

static void record_pointer(float x, float y, int clicks) {
    if (record) {
        fprintf(record, "pointer %lu %.9g %.9g %d\n", frame, x, y, clicks);
        fflush(record);
    }
}

/* menu.c: the mouse's pointer in a menu (PSP pixels; whether it moved, how many clicks since last asked) - the
 * window's (or MHFU_TEST_POINTER's), recorded, or the replay's */
int present_pointer(float *x, float *y, int *moved, int *clicks) {
    if (replaying()) {
        static Pointer at = {0, 0, 0, 0};
        static int seen;
        *moved = *clicks = 0;
        while (replay_pointer_at < replay_pointer_n && replay_pointer[replay_pointer_at].frame <= frame) {
            at = replay_pointer[replay_pointer_at++];
            seen = 1;
            if (at.frame == frame) {
                *moved = 1, *clicks += at.clicks;
                record_pointer(at.x, at.y, at.clicks);     /* (a recording that extends the replay) */
            }
        }
        *x = at.x;
        *y = at.y;
        return seen;
    }
    int point = menu_test_pointer(x, y, moved, clicks);
    if (!point) {
        point = window_pointer(x, y);
        *moved = window_pointer_moved();
        *clicks = window_pointer_clicks();
    }
    if (point && (*moved || *clicks))
        record_pointer(*x, *y, *clicks);
    return point;
}

/* The devices' buttons (and the script's, the intro's answer) */
static void buttons_now(void) {
    scripted_input(window_buttons());
    if (skip_intro() && intro_time() >= LANGUAGE_FRAME && intro_time() < LANGUAGE_FRAME + 5)
        ctrl_buttons |= 0x4000;
}

/* system.c: the game reading the controller - most of a frame after it showed one (it waits for its next), so the
 * buttons are taken again as they are now, the window's events seen first, and recorded again if they changed (for the
 * same frame: a replay has them all by then). The stick (and the camera the mouse turns) stays as at the frame. */
void present_input_read(void) {
    if (!input_due)
        return;
    input_due = 0;
    if (replaying()) {
        uint32_t none = 0;
        menu_input(&none);          /* (the menus noted all the same) */
        return;
    }
    if (!window_poll())
        exit(0);
    buttons_now();
    menu_input(&ctrl_buttons);      /* (the mouse's in a menu: once a frame, here) */
    record_input(0, 0);
}

void glyphs_frame_end(void);
int overlay_open(void);

static uint64_t flip_us;     /* (MHFU_FRAME_LOG: of the frame's time, what the renderer's flip took) */

/* MHFU_PROFILE=<file>,<from>,<to>: where the time goes between two game frames, by a timer's samples of the instruction pointer
 * (of the game's own thread and any other that has the signal), written as addresses for tools/profile.py to name */
#define PROFILE_SAMPLES (1 << 21)
static uint64_t *profile_at;
static volatile int profile_n;
static void profile_signal(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    if (profile_n < PROFILE_SAMPLES)
        profile_at[profile_n++] = (uint64_t)((ucontext_t *)context)->uc_mcontext.gregs[REG_RIP];
}
static void profile_frame(unsigned long f) {
    static int state = -1;
    static long from, to;
    static char path[1024];
    if (state < 0) {
        state = 0;
        const char *e = getenv("MHFU_PROFILE");
        if (e && sscanf(e, "%1023[^,],%ld,%ld", path, &from, &to) == 3) {
            state = 1;
            profile_at = malloc(PROFILE_SAMPLES * sizeof *profile_at);
        }
    }
    if (state == 1 && (long)f >= from) {
        struct sigaction sa = {0};
        sa.sa_sigaction = profile_signal;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, NULL);
        struct itimerval t = {{0, 500}, {0, 500}};
        setitimer(ITIMER_PROF, &t, NULL);
        state = 2;
    } else if (state == 2 && (long)f >= to) {
        struct itimerval t = {{0, 0}, {0, 0}};
        setitimer(ITIMER_PROF, &t, NULL);
        FILE *o = fopen(path, "wb");
        if (o) {
            fwrite(profile_at, sizeof *profile_at, (size_t)profile_n, o);
            fclose(o);
        }
        fprintf(stderr, "profile: %d samples, frames %ld to %ld\n", profile_n, from, to);
        state = 0;
    }
}

void present_frame(void) {
    glyphs_frame_end();             /* (the button hints drawn in this frame: those shown) */
    static uint64_t frame_wall;
    uint64_t wall = wall_time_us();
    if (frame_wall && wall - frame_wall > longest_frame_us)
        longest_frame_us = wall - frame_wall;
    frame_wall = wall;
    if (dump_every < 0) {
        const char *e = getenv("MHFU_DUMP");
        dump_every = e ? atoi(e) : 0;
        if (dump_every)
            mkdir("build/frames", 0755);
    }
    frame++;
    perf_game_frame(frame);
    pacing_tick(frame);
    capture_tick(frame);
    inspect_tick(frame);
    gpu_pick_frame(display_fb_addr);
    profile_frame(frame);
    /* MHFU_FRAME_LOG=<file>: a frame's wall time and which area the game is in, to see where it slows */
    {
        static FILE *frame_log;
        static int read;
        static uint64_t last;
        if (!read) {
            read = 1;
            if (getenv("MHFU_FRAME_LOG"))
                frame_log = fopen(getenv("MHFU_FRAME_LOG"), "w");
        }
        if (frame_log) {
            uint32_t game = *(uint32_t *)(rt_mem + 0x089CC558u);
            unsigned area = game >= 0x08800000u && game < 0x0A000000u ? *(uint16_t *)(rt_mem + game + 0x6AF0Eu) : 0;
            fprintf(frame_log, "%lu\t%llu\t%u\t%llu\n", frame, (unsigned long long)(wall - last), area, (unsigned long long)flip_us);
            flip_us = 0;
            last = wall;
            if (frame % 240 == 0) {
                void gpu_prim_stats_now(void);
                fprintf(stderr, "frame %lu, stage %u:\n", frame, area);
                gpu_prim_stats_now();
            }
        }
    }
    /* MHFU_FPS_STATS without a frame rate of the display's own: the game's (each of its frames shown) */
    static uint64_t stats_wall;
    static unsigned long stats_frame;
    if (!display_fps() && !fast_forward() && getenv("MHFU_FPS_STATS")) {
        if (!stats_wall)
            stats_wall = wall, stats_frame = frame;
        if (wall - stats_wall > 5000000) {
            double secs = (double)(wall - stats_wall) / 1e6;
            unsigned long smoothed, drawn;
            gpu_native_stats(&smoothed, &drawn);
            fprintf(stderr, "the game: %.1f frames a second (longest %.1f ms), idle %.0f%%, %.0f%% of draws smoothed, "
                    "%lu frames left out (late; drawing 1 of %d)\n", (double)(frame >= stats_frame ? frame - stats_frame : 0) / secs,
                    (double)longest_frame_us / 1000, (double)time_slept_us() / 1e4 / secs,
                    drawn ? 100.0 * (double)smoothed / (double)drawn : 0.0, native_skipped, gpu_divisor);
            native_skipped = 0;
            stats_wall = wall;
            stats_frame = frame;
            longest_frame_us = 0;
        }
    }
    static long debug_frame = -1;
    if (debug_frame < 0)
        debug_frame = getenv("MHFU_DEBUG_FRAME") ? atol(getenv("MHFU_DEBUG_FRAME")) : 0;
    if (debug_frame && (long)frame == debug_frame)
        debug_frame_hook(frame);
    tick_frame(frame);
    gpu_frame_tick();
    watch();
    snapshots();
    uint32_t *pixels = window_pixels();
    static long dump_from = -1;     /* MHFU_DUMP_FROM=N: none before frame N */
    if (dump_from < 0)
        dump_from = getenv("MHFU_DUMP_FROM") ? atol(getenv("MHFU_DUMP_FROM")) : 0;
    int dump = dump_every && frame % (unsigned long)dump_every == 0 && (long)frame >= dump_from;
    if (fast_forward()) {
        if (display_fps())
            gpu_frame_end(display_fb_addr, time_us());
        if (dump && (!display_fps() || gpu_render_frame(1.0f)) && render_on)
            render_readback(display_fb_addr, pixels);
        if (capture_wants_shot() && render_on && (!display_fps() || gpu_render_frame(1.0f)))
            capture_take_shot(display_fb_addr, NULL);
        if (render_on && !display_fps())
            render_finish_frame();
    } else if (display_fps()) {
        gpu_frame_end(display_fb_addr, time_us());
        if (dump && gpu_render_frame(1.0f)) {
            render_readback(display_fb_addr, pixels);
            dump_full(display_fb_addr, frame);
        }
    } else if (render_on) {
        if (dump) {
            render_readback(display_fb_addr, pixels);
            dump_full(display_fb_addr, frame);
        }
        if (!gpu_skip_draw) {
            uint64_t before = wall_time_us();
            if (capture_wants_shot())
                capture_take_shot(display_fb_addr, NULL);
            flip(display_fb_addr);
            flip_us += wall_time_us() - before;
        } else {
            native_skipped++;
            perf_skipped++;
        }
        if (!gpu_skip_draw && getenv("MHFU_NATIVE_DEBUG"))
            fprintf(stderr, "flip: %.3f\n", rt_game_30ths());
        /* the game at its own rate above 30, well behind (four frames, as it shows one - its own work in it): the next
         * frame left out, not two running - so that it keeps time, and its sound with it; nothing kept from one frame
         * to the next, so that it never stays drawing fewer (gpu.c) */
        static int skips = -1;          /* MHFU_NATIVE_SKIP=0: never */
        if (skips < 0)
            skips = !getenv("MHFU_NATIVE_SKIP") || atoi(getenv("MHFU_NATIVE_SKIP")) != 0;
        int64_t frame_us = (int64_t)(1000000.0f * rt_frame_30ths() / 30.0f);
        gpu_divisor = skips && gpu_native_smoothing() && !fast_forward() && !gpu_skip_draw &&
                      clock_behind_us() > 4 * frame_us ? 2 : 1;
    } else {
        convert(pixels);
        if (capture_wants_shot())
            capture_take_shot(0, pixels);
        window_show(pixels);
        perf_shown();
    }
    if (!window_poll())
        exit(0);
    /* the controls screen (F1), or the inspector's hold (F8): the game paused, the frame shown again under it - until a
     * capture asked for now has what it needs (its state: the game must get to a point where every thread waits) */
    while (overlay_open() || (inspect_paused() && !capture_state_pending())) {
        if (!window_poll())
            exit(0);
        uint32_t shown = display_fps() ? gpu_render_frame(1.0f) : display_fb_addr;
        if (render_on && shown && capture_wants_shot())
            capture_take_shot(shown, NULL);
        if (render_on && shown)
            flip(shown);
        struct timespec ts = {0, 16000000};
        nanosleep(&ts, NULL);
    }
    /* the input the game sees this frame: the devices', a script's, the intro's answer, or the replay's (the buttons
     * taken again as the game reads them: present_input_read) */
    if (input_due) {                /* (not read last frame: the menus noted all the same) */
        uint32_t none = 0;
        menu_input(&none);
    }
    input_due = 1;
    window_stick(&ctrl_lx, &ctrl_ly);
    float mx, my;
    window_mouse(&mx, &my);
    buttons_now();
    replay_input(&mx, &my);
    replay_pokes();
    int orbit_drawn = display_fps() != 0 || gpu_native_smoothing();   /* (with MHFU_FPS, or at the native frame rate) */
    if (camera_orbit_ready != orbit_drawn + 1)
        camera_orbit_ready = orbit_drawn + 1, camera_orbit_enable(orbit_drawn);
    rt_camera_free = frame >= camera_free_from();
    if (!replaying())
        camera_free_look(controls_free_look(), &mx, &my);
    camera_update(mx, my);
    if (!replaying())
        camera_stick(&ctrl_lx, &ctrl_ly);          /* (a replay has the stick as the game had it) */
    record_input(mx, my);
    if (dump) {
        char path[64];
        snprintf(path, sizeof path, "build/frames/%06lu.png", frame);
        write_png(path, pixels);
    }
}
