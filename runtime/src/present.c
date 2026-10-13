/* Presenting frames: in the window (window.c), and written out as PNGs (MHFU_DUMP=N: every Nth frame, to
 * build/frames/), to compare with PPSSPP's. */
#include "hle.h"
#include "render.h"

#include <png.h>
#include <stdlib.h>
#include <sys/stat.h>

static int dump_every = -1;
static unsigned long frame;

/* Scripted input, for running unattended: MHFU_INPUT="first-last:buttons,..." holds the buttons (PSP button bits, in
 * hex: 0x4000 cross, 0x2000 circle, 0x8 start, 0x10 up, 0x40 down...) from frame first to frame last */
extern uint32_t ctrl_buttons;

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
    ctrl_buttons = buttons;
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

void present_frame(void) {
    if (dump_every < 0) {
        const char *e = getenv("MHFU_DUMP");
        dump_every = e ? atoi(e) : 0;
        if (dump_every)
            mkdir("build/frames", 0755);
    }
    frame++;
    watch();
    uint32_t *pixels = window_pixels();
    int dump = dump_every && frame % (unsigned long)dump_every == 0;
    if (render_on) {
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
    if (dump) {
        char path[64];
        snprintf(path, sizeof path, "build/frames/%06lu.png", frame);
        write_png(path, pixels);
    }
}
