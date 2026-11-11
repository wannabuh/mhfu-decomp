/* KTX2 images for texture packs (texpack.c), as PPSSPP reads them: Basis Universal's (UASTC or ETC1S, Zstandard or
 * BasisLZ inside), turned into RGBA by its transcoder (the basis-universal package's). Level 0 only: the smaller
 * levels are made here as for every texture. Safe from several threads at once: each call has a transcoder of its own,
 * with the state inside it (a separate ktx2_transcoder_state failed every other image with the packaged library). */
#include <basisu/transcoder/basisu_transcoder.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>

extern "C" uint32_t *ktx2_read(const char *path, int *w, int *h);

static std::once_flag inited;

uint32_t *ktx2_read(const char *path, int *w, int *h) {
    std::call_once(inited, [] { basist::basisu_transcoder_init(); });
    FILE *f = fopen(path, "rb");
    if (!f)
        return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return nullptr;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)n);
    size_t got = data ? fread(data, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n) {
        free(data);
        return nullptr;
    }
    uint32_t *px = nullptr;
    {
        basist::ktx2_transcoder tr;
        if (tr.init(data, (uint32_t)n) && tr.start_transcoding()) {
            uint32_t iw = tr.get_width(), ih = tr.get_height();
            if (iw && ih && iw <= 8192 && ih <= 8192) {
                px = (uint32_t *)malloc((size_t)iw * ih * 4);
                if (px && tr.transcode_image_level(0, 0, 0, px, iw * ih, basist::transcoder_texture_format::cTFRGBA32,
                                                   0, 0, 0, -1, -1, nullptr)) {
                    *w = (int)iw;
                    *h = (int)ih;
                } else {
                    free(px);
                    px = nullptr;
                }
            }
        }
    }
    free(data);
    return px;
}
