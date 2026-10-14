/* Presenting frames: in the window (window.c), and written out as PNGs (MHFU_DUMP=N: every Nth frame, to
 * build/frames/), to compare with PPSSPP's.
 *
 * MHFU_FPS=N (with the Vulkan renderer) shows N frames a second, whatever the game's 30: the game's frames are
 * recorded (gpu.c), and the display draws its own from them - interpolated between the last two - in the scheduler's
 * idle time (present_pump). */
#include "hle.h"
#include "render.h"

#include <png.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int dump_every = -1;
static unsigned long frame;
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
 * sensitivity, MHFU_MOUSE_SENS, as the mouse turns the camera in the game), then a line for each frame whose
 * input differs from the one before or moves the mouse: "<frame> <buttons> <stick x> <stick y> <mouse x> <mouse y>",
 * and "end <frame>": the input as the game had it (the stick as turned by the mouse's orbit, camera.c). A save changed
 * since, or a runtime that runs the game differently, can make a replay go astray. */
typedef struct { unsigned long frame; uint32_t buttons; int lx, ly; float mx, my; } Input;
static Input *replay;
static size_t replay_n, replay_at;
static unsigned long replay_end;
static int replay_loaded;
static uint64_t replay_epoch_us;

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

int fast_forward(void) {
    return (skip_intro() && frame >= LANGUAGE_FRAME + 10 && frame < TITLE_FRAME) || (replaying() && frame < replay_speed_at());
}

/* This frame's input, from the replay while there is one */
static void replay_input(float *mx, float *my) {
    if (!replaying())
        return;
    static Input held = {0, 0, 128, 128, 0, 0};
    *mx = *my = 0;
    while (replay_at < replay_n && replay[replay_at].frame <= frame) {
        held = replay[replay_at];
        if (held.frame == frame) {
            *mx = held.mx;
            *my = held.my;
        }
        replay_at++;
    }
    ctrl_buttons = held.buttons;
    ctrl_lx = (uint8_t)held.lx;
    ctrl_ly = (uint8_t)held.ly;
    if (frame + 1 == replay_end)
        fprintf(stderr, "replay done: over to you\n");
}

static FILE *record;
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
                fprintf(record, "mhfu-replay 1\nepoch %llu\nsens %.9g\n", (unsigned long long)run_epoch_us(),
                        camera_sens());
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

static void write_png(const char *path, const uint32_t *frame) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    png_init_io(png, f);
    png_set_IHDR(png, info, 480, 272, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < 272; y++)
        png_write_row(png, (png_bytep)(frame + y * 480));
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
}

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

uint64_t wall_time_us(void), time_slept_us(void);
static uint64_t longest_frame_us;    /* the longest a game frame took, on the wall clock (for MHFU_FPS_STATS) */
static int fps = -1;                /* the display's frame rate; 0: the game's own frames */
static uint64_t next_display;

static int display_fps(void) {
    if (fps < 0) {
        const char *e = getenv("MHFU_FPS");
        fps = e && render_on ? atoi(e) : 0;
        if (fps < 0 || fps > 1000)
            fps = 0;
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
    uint64_t now = time_us(), interval = 1000000u / (uint64_t)fps;
    if (now < next_display)
        return next_display;
    /* the input first, so that the frame shows the mouse as it is now (as the game's frames do) */
    if (!window_poll())
        exit(0);
    /* the moment shown trails the game by one frame: t runs from 0, as a frame lands, towards 1 */
    uint64_t since = now - gpu_last_frame_time(), period = gpu_frame_period();
    float t = period ? (float)since / (float)period : 1.0f;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    uint32_t addr = gpu_render_frame(t);
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
        }
        gpu_render_frame(t);
    }
    if (addr)
        render_flip(addr);
    static uint64_t stats_at;
    static unsigned long shown, stats_frame;
    shown++;
    if (getenv("MHFU_FPS_STATS")) {
        uint64_t wall = wall_time_us();
        if (!stats_at)
            stats_at = wall;
        if (wall - stats_at > 5000000) {
            unsigned long m, u;
            gpu_match_stats(&m, &u);
            double secs = (double)(wall - stats_at) / 1e6;
            fprintf(stderr, "display: %.1f frames a second, the game %.1f (longest frame %.1f ms), idle %.0f%%, "
                    "%.0f%% of primitives matched\n", (double)shown / secs, (double)(frame - stats_frame) / secs,
                    (double)longest_frame_us / 1000, (double)time_slept_us() / 1e4 / secs,
                    m + u ? 100.0 * (double)m / (double)(m + u) : 0.0);
            stats_at = wall;
            stats_frame = frame;
            longest_frame_us = 0;
            shown = 0;
        }
    }
    next_display += interval;
    if (next_display < now)
        next_display = now + interval;
    return next_display;
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

void present_frame(void) {
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
    static long debug_frame = -1;
    if (debug_frame < 0)
        debug_frame = getenv("MHFU_DEBUG_FRAME") ? atol(getenv("MHFU_DEBUG_FRAME")) : 0;
    if (debug_frame && (long)frame == debug_frame)
        debug_frame_hook(frame);
    tick_frame(frame);
    watch();
    snapshots();
    uint32_t *pixels = window_pixels();
    int dump = dump_every && frame % (unsigned long)dump_every == 0;
    if (fast_forward()) {
        if (display_fps())
            gpu_frame_end(display_fb_addr, time_us());
        if (dump && (!display_fps() || gpu_render_frame(1.0f)) && render_on)
            render_readback(display_fb_addr, pixels);
        if (render_on && !display_fps())
            render_finish_frame();
    } else if (display_fps()) {
        gpu_frame_end(display_fb_addr, time_us());
        if (dump && gpu_render_frame(1.0f))
            render_readback(display_fb_addr, pixels);
    } else if (render_on) {
        if (dump)
            render_readback(display_fb_addr, pixels);
        render_flip(display_fb_addr);
    } else {
        convert(pixels);
        window_show(pixels);
    }
    if (!window_poll())
        exit(0);
    /* the input the game sees this frame: the devices', a script's, the intro's answer, or the replay's */
    window_stick(&ctrl_lx, &ctrl_ly);
    float mx, my;
    window_mouse(&mx, &my);
    scripted_input(window_buttons());
    if (skip_intro() && frame >= LANGUAGE_FRAME && frame < LANGUAGE_FRAME + 5)
        ctrl_buttons |= 0x4000;
    replay_input(&mx, &my);
    if (!camera_orbit_ready)
        camera_orbit_ready = 1, camera_orbit_enable(display_fps() != 0);      /* the orbit is only drawn with MHFU_FPS */
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
