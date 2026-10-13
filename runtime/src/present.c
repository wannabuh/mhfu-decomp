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
#include <sys/stat.h>

static int dump_every = -1;
static unsigned long frame;

/* Scripted input, for running unattended: MHFU_INPUT="first-last:buttons,..." holds the buttons (PSP button bits, in
 * hex: 0x4000 cross, 0x2000 circle, 0x8 start, 0x10 up, 0x40 down...) from frame first to frame last; and the analog
 * stick, pushed fully: 0x10000 up, 0x20000 down, 0x40000 left, 0x80000 right */
extern uint32_t ctrl_buttons;
extern uint8_t ctrl_lx, ctrl_ly;

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
void camera_update(void);
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
    if (!display_fps())
        return ~(uint64_t)0;
    uint64_t now = time_us(), interval = 1000000u / (uint64_t)fps;
    if (now < next_display)
        return next_display;
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
    static unsigned long shown;
    shown++;
    if (getenv("MHFU_FPS_STATS") && now - stats_at > 5000000) {
        unsigned long m, u;
        gpu_match_stats(&m, &u);
        fprintf(stderr, "display: %.1f frames a second, %.0f%% of primitives matched\n",
                (double)shown * 1e6 / (double)(now - stats_at), m + u ? 100.0 * (double)m / (double)(m + u) : 0.0);
        stats_at = now;
        shown = 0;
    }
    if (!window_poll())
        exit(0);
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
    if (dump_every < 0) {
        const char *e = getenv("MHFU_DUMP");
        dump_every = e ? atoi(e) : 0;
        if (dump_every)
            mkdir("build/frames", 0755);
    }
    frame++;
    watch();
    snapshots();
    uint32_t *pixels = window_pixels();
    int dump = dump_every && frame % (unsigned long)dump_every == 0;
    if (display_fps()) {
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
    scripted_input(window_buttons());
    camera_update();
    if (dump) {
        char path[64];
        snprintf(path, sizeof path, "build/frames/%06lu.png", frame);
        write_png(path, pixels);
    }
}
