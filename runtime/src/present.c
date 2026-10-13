/* Presenting frames. For now: written out as PNGs (MHFU_DUMP=N: every Nth frame, to build/frames/), to compare with
 * PPSSPP's. The window comes with the Vulkan renderer. */
#include "hle.h"

#include <png.h>
#include <stdlib.h>
#include <sys/stat.h>

static int dump_every = -1;
static unsigned long frame;

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

static void write_png(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    png_init_io(png, f);
    png_set_IHDR(png, info, 480, 272, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    static uint32_t row[480];
    int bpp = display_fb_format == 3 ? 4 : 2;
    for (int y = 0; y < 272; y++) {
        for (int x = 0; x < 480; x++)
            row[x] = pixel(display_fb_addr + (uint32_t)((y * (int)display_fb_width + x) * bpp), (int)display_fb_format);
        png_write_row(png, (png_bytep)row);
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
}

void present_frame(void) {
    if (dump_every < 0) {
        const char *e = getenv("MHFU_DUMP");
        dump_every = e ? atoi(e) : 0;
        if (dump_every)
            mkdir("build/frames", 0755);
    }
    frame++;
    if (dump_every && frame % (unsigned long)dump_every == 0) {
        char path[64];
        snprintf(path, sizeof path, "build/frames/%06lu.png", frame);
        write_png(path);
    }
}
