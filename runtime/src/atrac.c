/* sceAtrac: ATRAC3 / ATRAC3+ decoding (the game's music and much of its sound), with FFmpeg's decoders.
 *
 * The game hands over a whole file (RIFF / WAVE: "fmt " - ATRAC3 is format 0x270, ATRAC3+ the extensible format with
 * its GUID -, "fact" - the samples and the encoder's delay -, "smpl" - loop points -, "data" - fixed-size frames), and
 * decodes it a frame at a time into 16-bit stereo. Only whole files in memory, which is how the game uses it (no
 * streaming). */
#include "hle.h"
#include "savestate.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <string.h>

#define MAX_IDS 8
#define ERROR_NO_ID          0x80630003u
#define ERROR_ALL_DATA_DECODED 0x80630024u
#define ERROR_SECOND_BUFFER_NOT_NEEDED 0x80630022u

typedef struct {
    int used;
    uint32_t buf, size;
    int codec;                      /* 0x270 ATRAC3, 0xFFFE ATRAC3+ */
    int channels, rate, block;      /* block: bytes a frame */
    uint32_t data_off, data_size;
    int frame_samples;              /* 1024 (ATRAC3) or 2048 (ATRAC3+) */
    int total_samples, skip;        /* from "fact" */
    int loop_start, loop_end, has_loop;
    int loop_num;                   /* loops left: -1 forever */
    int frame;                      /* the next frame to decode */
    int sample;                     /* the next sample to hand out (a position in the file's samples, its delay too) */
    int16_t pcm[2048 * 2];          /* the frame decoded last: pcm_n samples from pcm_start */
    int pcm_start, pcm_n;
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *out;
} Atrac;

SAVED static Atrac ids[MAX_IDS];   /* (its decoders the host's: made again after a load) */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static int parse(Atrac *a) {
    const uint8_t *d = PTR(a->buf);
    if (memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4))
        return 0;
    const uint8_t *fmt = NULL;
    uint32_t fmt_size = 0;
    for (uint32_t off = 12; off + 8 <= a->size;) {
        uint32_t len = rd32(d + off + 4);
        const uint8_t *body = d + off + 8;
        if (!memcmp(d + off, "fmt ", 4)) {
            fmt = body;
            fmt_size = len;
        } else if (!memcmp(d + off, "fact", 4) && len >= 8) {
            a->total_samples = (int)rd32(body);
            a->skip = (int)rd32(body + 4);
        } else if (!memcmp(d + off, "smpl", 4) && len >= 36 + 24 && rd32(body + 28)) {
            a->loop_start = (int)rd32(body + 36 + 8);
            a->loop_end = (int)rd32(body + 36 + 12);
            a->has_loop = 1;
        } else if (!memcmp(d + off, "data", 4)) {
            a->data_off = off + 8;
            a->data_size = len;
            break;
        }
        off += 8 + ((len + 1) & ~1u);
    }
    if (!fmt || !a->data_off)
        return 0;
    a->codec = rd16(fmt);
    a->channels = rd16(fmt + 2);
    a->rate = (int)rd32(fmt + 4);
    a->block = rd16(fmt + 12);
    a->frame_samples = a->codec == 0x270 ? 1024 : 2048;
    const AVCodec *codec = avcodec_find_decoder(a->codec == 0x270 ? AV_CODEC_ID_ATRAC3 : AV_CODEC_ID_ATRAC3P);
    if (!codec)
        return 0;
    a->ctx = avcodec_alloc_context3(codec);
    av_channel_layout_default(&a->ctx->ch_layout, a->channels);
    a->ctx->sample_rate = a->rate;
    a->ctx->block_align = a->block;
    if (a->codec == 0x270 && fmt_size >= 18 + 14) {            /* ATRAC3's extradata: the 14 bytes after cbSize */
        a->ctx->extradata = av_mallocz(14 + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(a->ctx->extradata, fmt + 18, 14);
        a->ctx->extradata_size = 14;
    }
    if (avcodec_open2(a->ctx, codec, NULL) < 0)
        return 0;
    a->pkt = av_packet_alloc();
    a->out = av_frame_alloc();
    return 1;
}

static Atrac *get(uint32_t id) {
    return id < MAX_IDS && ids[id].used ? &ids[id] : NULL;
}

static void release(Atrac *a) {
    if (a->ctx)
        avcodec_free_context(&a->ctx);
    if (a->pkt)
        av_packet_free(&a->pkt);
    if (a->out)
        av_frame_free(&a->out);
    memset(a, 0, sizeof *a);
}

static int total_frames(const Atrac *a) { return (int)(a->data_size / (uint32_t)a->block); }

/* (buffer, size) -> an id */
void hle_sceAtracSetDataAndGetID(Ctx *c) {
    for (uint32_t i = 0; i < MAX_IDS; i++)
        if (!ids[i].used) {
            Atrac *a = &ids[i];
            memset(a, 0, sizeof *a);
            a->used = 1;
            a->buf = ARG0;
            a->size = ARG1;
            a->loop_num = 0;
            if (!parse(a)) {
                HLE_WARN("sceAtracSetDataAndGetID: not ATRAC data I can decode");
                release(a);
                RET(0x80630006u);
                return;
            }
            HLE_LOG("atrac %u: %s, %d channels, %d Hz, %d-byte frames, %d frames, %d samples after a delay of %d, loop %d-%d", i,
                    a->codec == 0x270 ? "ATRAC3" : "ATRAC3+", a->channels, a->rate, a->block, total_frames(a), a->total_samples, a->skip,
                    a->loop_start, a->loop_end);
            RET(i);
            return;
        }
    RET(ERROR_NO_ID);
}

void hle_sceAtracReleaseAtracID(Ctx *c) {
    Atrac *a = get(ARG0);
    if (a)
        release(a);
    RET(a ? 0 : ERROR_NO_ID);
}

void hle_sceAtracSetLoopNum(Ctx *c) {
    Atrac *a = get(ARG0);
    if (a)
        a->loop_num = (int32_t)ARG1 == 0x7FFFFFFF ? -1 : (int32_t)ARG1;
    RET(a ? 0 : ERROR_NO_ID);
}

/* Where the samples end: the loop's end while it loops (the "smpl" points are positions in the file's samples, the
 * encoder's delay included), the data's end otherwise */
static int looping(const Atrac *a) { return a->has_loop && a->loop_num != 0 && a->loop_start < a->loop_end; }

static int end_sample(const Atrac *a) {
    int end = total_frames(a) * a->frame_samples;
    if (a->total_samples && a->total_samples + a->skip < end)
        end = a->total_samples + a->skip;
    return end;
}

/* The next frame into pcm[] (silence if it does not decode); 0 past the data */
static int decode_frame(Atrac *a) {
    if (a->frame >= total_frames(a))
        return 0;
    a->pkt->data = (uint8_t *)PTR(a->buf + a->data_off + (uint32_t)(a->frame * a->block));
    a->pkt->size = a->block;
    a->pcm_start = a->frame * a->frame_samples;
    a->pcm_n = a->frame_samples;
    memset(a->pcm, 0, sizeof a->pcm);
    if (avcodec_send_packet(a->ctx, a->pkt) >= 0 && avcodec_receive_frame(a->ctx, a->out) >= 0) {
        int ch = a->out->ch_layout.nb_channels, n = a->out->nb_samples < 2048 ? a->out->nb_samples : 2048;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < 2; k++) {
                const float *src = (const float *)a->out->extended_data[k < ch ? k : 0];
                float v = src[i] * 32767.0f;
                a->pcm[i * 2 + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
    }
    a->frame++;
    return 1;
}

/* To a sample: the decoder starts again at the frame before its frame (a frame's samples overlap the one before) */
static void seek(Atrac *a, int sample) {
    avcodec_flush_buffers(a->ctx);
    int f = sample / a->frame_samples;
    a->frame = f > 0 ? f - 1 : 0;
    if (f > 0)
        decode_frame(a);
    decode_frame(a);
    a->sample = sample;
}

/* A state about to be loaded (savestate.c): the decoders there are, freed - the ids loaded have none yet */
void atrac_state_loading(void) {
    for (int i = 0; i < MAX_IDS; i++)
        if (ids[i].used) {
            if (ids[i].ctx)
                avcodec_free_context(&ids[i].ctx);
            if (ids[i].pkt)
                av_packet_free(&ids[i].pkt);
            if (ids[i].out)
                av_frame_free(&ids[i].out);
        }
}

/* A state loaded: each id's decoder made again, and brought to where it was by decoding the two frames before its
 * next (a frame's samples overlap the one before's) - the samples it was handing out kept */
void atrac_state_loaded(void) {
    for (int i = 0; i < MAX_IDS; i++) {
        Atrac *a = &ids[i];
        a->ctx = NULL;
        a->pkt = NULL;
        a->out = NULL;
        if (!a->used)
            continue;
        int frame = a->frame, start = a->pcm_start, n = a->pcm_n;
        int16_t pcm[2048 * 2];
        memcpy(pcm, a->pcm, sizeof pcm);
        if (!parse(a)) {
            a->used = 0;
            continue;
        }
        for (a->frame = frame >= 2 ? frame - 2 : 0; a->frame < frame;)
            decode_frame(a);
        a->frame = frame;
        a->pcm_start = start;
        a->pcm_n = n;
        memcpy(a->pcm, pcm, sizeof pcm);
    }
}

/* (id, out, samples*, finished*, remaining frames*): a frame's worth of samples, 16-bit interleaved - across the loop
 * point, sample for sample, while it loops */
void hle_sceAtracDecodeData(Ctx *c) {
    Atrac *a = get(ARG0);
    if (!a) {
        RET(ERROR_NO_ID);
        return;
    }
    if (!looping(a) && a->sample >= end_sample(a)) {
        if (ARG2) SW(ARG2, 0);
        if (ARG3) SW(ARG3, 1);
        RET(ERROR_ALL_DATA_DECODED);
        return;
    }
    int16_t *out = PTR(ARG1);
    int n = 0;
    while (n < a->frame_samples) {
        if (looping(a) && a->sample > a->loop_end) {
            seek(a, a->loop_start);
            if (a->loop_num > 0)
                a->loop_num--;
            continue;
        }
        int end = looping(a) ? a->loop_end + 1 : end_sample(a);
        if (a->sample >= end)
            break;
        if (a->sample < a->pcm_start || a->sample >= a->pcm_start + a->pcm_n) {
            if (!decode_frame(a))
                break;
            continue;
        }
        int k = a->pcm_start + a->pcm_n - a->sample;
        if (k > end - a->sample)
            k = end - a->sample;
        if (k > a->frame_samples - n)
            k = a->frame_samples - n;
        memcpy(out + 2 * n, a->pcm + 2 * (a->sample - a->pcm_start), (size_t)k * 4);
        n += k;
        a->sample += k;
    }
    int finished = !looping(a) && a->sample >= end_sample(a);
    if (ARG2) SW(ARG2, (uint32_t)n);
    if (ARG3) SW(ARG3, (uint32_t)finished);
    if (ARG4) SW(ARG4, 0xFFFFFFFFu);        /* all the data is in memory */
    HLE_LOG("sceAtracDecodeData(%u): %d samples, at %d, finished %d", ARG0, n, a->sample, finished);
    RET(0);
}

void hle_sceAtracGetRemainFrame(Ctx *c) {
    if (ARG1) SW(ARG1, 0xFFFFFFFFu);        /* -1: all the data is in memory */
    RET(get(ARG0) ? 0 : ERROR_NO_ID);
}

void hle_sceAtracGetNextDecodePosition(Ctx *c) {
    Atrac *a = get(ARG0);
    if (!a) {
        RET(ERROR_NO_ID);
        return;
    }
    if (!looping(a) && a->sample >= end_sample(a)) {
        RET(ERROR_ALL_DATA_DECODED);
        return;
    }
    if (ARG1) SW(ARG1, (uint32_t)a->sample);
    RET(0);
}

/* (id, write pointer*, writable bytes*, read offset*): nothing to add - all the data is there */
void hle_sceAtracGetStreamDataInfo(Ctx *c) {
    Atrac *a = get(ARG0);
    if (!a) {
        RET(ERROR_NO_ID);
        return;
    }
    if (ARG1) SW(ARG1, a->buf);
    if (ARG2) SW(ARG2, 0);
    if (ARG3) SW(ARG3, a->size);
    RET(0);
}

/* (id, end sample*, loop start*, loop end*): samples counted after the encoder's delay; -1 for no loop */
void hle_sceAtracGetSoundSample(Ctx *c) {
    Atrac *a = get(ARG0);
    if (!a) {
        RET(ERROR_NO_ID);
        return;
    }
    int end = a->total_samples ? a->total_samples - 1 : total_frames(a) * a->frame_samples - a->skip - 1;
    if (ARG1) SW(ARG1, (uint32_t)end);
    if (ARG2) SW(ARG2, a->has_loop ? (uint32_t)a->loop_start : 0xFFFFFFFFu);
    if (ARG3) SW(ARG3, a->has_loop ? (uint32_t)a->loop_end : 0xFFFFFFFFu);
    RET(0);
}

/* (id, loops left*, looping*) */
void hle_sceAtracGetLoopStatus(Ctx *c) {
    Atrac *a = get(ARG0);
    if (!a) {
        RET(ERROR_NO_ID);
        return;
    }
    if (ARG1) SW(ARG1, (uint32_t)a->loop_num);
    if (ARG2) SW(ARG2, a->has_loop ? 1 : 0);
    RET(0);
}

void hle_sceAtracGetBitrate(Ctx *c) {
    Atrac *a = get(ARG0);
    if (!a) {
        RET(ERROR_NO_ID);
        return;
    }
    if (ARG1) SW(ARG1, (uint32_t)(a->block * 8 * a->rate / a->frame_samples / 1000));
    RET(0);
}
