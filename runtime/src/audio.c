/* Audio: the output channels, and the SAS (the PSP's software synthesiser the game's sound engine drives).
 *
 * Output is timed as the hardware plays it - a channel takes one buffer at a time, and outputting the next blocks until
 * the one before has played, at 44.1 kHz - and heard through SDL: each channel a stream on the default device, which
 * mixes them (none in headless runs). Music comes through ATRAC (atrac.c), sound effects through the SAS (below). */
#include "hle.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define CHANNELS 8
#define RATE 44100

static struct { int reserved; uint32_t samples, format, vol_l, vol_r; uint64_t queued_until; SDL_AudioStream *stream; }
    chan[CHANNELS];
static SDL_AudioDeviceID device;
static int audio_state;             /* 0 not tried, 1 on, -1 off */

static int audio_on(void) {
    if (!audio_state) {
        audio_state = -1;
        if (getenv("MHFU_HEADLESS") || getenv("MHFU_MUTE"))
            return 0;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "no sound: %s\n", SDL_GetError());
            return 0;
        }
        SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, RATE};
        device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
        if (!device) {
            fprintf(stderr, "no sound: %s\n", SDL_GetError());
            return 0;
        }
        audio_state = 1;
    }
    return audio_state > 0;
}

/* MHFU_AUDIO_DUMP=1: each channel's output (as given, before volumes) to build/audio-ch<N>.raw, 16-bit */
static void dump(int ch, uint32_t addr) {
    static FILE *files[CHANNELS];
    static int on = -1;
    if (on < 0)
        on = getenv("MHFU_AUDIO_DUMP") != NULL;
    if (!on)
        return;
    if (!files[ch]) {
        char path[64];
        snprintf(path, sizeof path, "build/audio-ch%d.raw", ch);
        files[ch] = fopen(path, "wb");
        if (!files[ch])
            return;
    }
    fwrite(PTR(addr), 1, chan[ch].samples * (chan[ch].format == 0x10 ? 2 : 4), files[ch]);
}

/* A buffer to the host: scaled by the channel's volumes (0x8000 is full), mono made stereo */
int fast_forward(void);

static void play(int ch, uint32_t addr, uint32_t vol_l, uint32_t vol_r) {
    if (!addr)                      /* no buffer: silence (the game outputs that when it stops a sound) */
        return;
    dump(ch, addr);
    if (!audio_on() || fast_forward())
        return;
    if (!chan[ch].stream) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, RATE};
        chan[ch].stream = SDL_CreateAudioStream(&spec, NULL);
        SDL_BindAudioStream(device, chan[ch].stream);
    }
    static int16_t buf[4096 * 2];
    uint32_t n = chan[ch].samples > 4096 ? 4096 : chan[ch].samples;
    int mono = chan[ch].format == 0x10;
    const int16_t *src = PTR(addr);
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = mono ? src[i] : src[2 * i], r = mono ? src[i] : src[2 * i + 1];
        buf[2 * i] = (int16_t)(l * (int32_t)vol_l / 0x8000);
        buf[2 * i + 1] = (int16_t)(r * (int32_t)vol_r / 0x8000);
    }
    SDL_PutAudioStreamData(chan[ch].stream, buf, (int)(n * 4));
}

static uint64_t duration_us(uint32_t samples) { return (uint64_t)samples * 1000000u / RATE; }

/* (channel or -1 for any, samples, format: 0 stereo, 0x10 mono) */
void hle_sceAudioChReserve(Ctx *c) {
    int ch = (int32_t)ARG0;
    if (ch < 0) {
        for (ch = CHANNELS - 1; ch >= 0 && chan[ch].reserved; ch--)
            ;
        if (ch < 0) {
            RET(0x80260002u);       /* no channel */
            return;
        }
    } else if (ch >= CHANNELS || chan[ch].reserved) {
        RET(0x80260002u);
        return;
    }
    chan[ch].reserved = 1;
    chan[ch].samples = ARG1;
    chan[ch].format = ARG2;
    chan[ch].queued_until = 0;
    RET(ch);
}

void hle_sceAudioChRelease(Ctx *c) {
    if (ARG0 < CHANNELS)
        chan[ARG0].reserved = 0;
    RET(0);
}

void hle_sceAudioSetChannelDataLen(Ctx *c) {
    if (ARG0 < CHANNELS)
        chan[ARG0].samples = ARG1;
    RET(0);
}

void hle_sceAudioChangeChannelConfig(Ctx *c) {
    if (ARG0 < CHANNELS)
        chan[ARG0].format = ARG1;
    RET(0);
}

void hle_sceAudioChangeChannelVolume(Ctx *c) {
    if (ARG0 < CHANNELS) {
        chan[ARG0].vol_l = ARG1;
        chan[ARG0].vol_r = ARG2;
    }
    RET(0);
}

/* (channel, left volume, right volume, buffer) */
void hle_sceAudioOutputPannedBlocking(Ctx *c) {
    uint32_t ch = ARG0;
    if (ch >= CHANNELS || !chan[ch].reserved) {
        RET(0x80260008u);           /* not reserved */
        return;
    }
    uint64_t d = duration_us(chan[ch].samples);
    /* one buffer can be queued behind the one playing: wait until there is room */
    if (chan[ch].queued_until > time_us() + d)
        thread_delay_until(chan[ch].queued_until - d);
    uint64_t now = time_us();
    chan[ch].queued_until = (chan[ch].queued_until > now ? chan[ch].queued_until : now) + d;
    play((int)ch, ARG3, ARG1, ARG2);
    RET(0);
}

void hle_sceAudioGetChannelRestLength(Ctx *c) {
    uint32_t ch = ARG0;
    if (ch >= CHANNELS) {
        RET(0x80260003u);
        return;
    }
    uint64_t now = time_us();
    uint64_t left = chan[ch].queued_until > now ? chan[ch].queued_until - now : 0;
    RET((uint32_t)(left * RATE / 1000000u));
}

/* -- SAS ---------------------------------------------------------------------------------------------------------- */

/* The PSP's synthesiser, as the game's sound engine drives it for sound effects: voices play VAG ADPCM samples
 * (PlayStation SPU format: 16-byte blocks - shift and filter, flags, 28 4-bit samples - with loop flags) at a pitch
 * (0x1000: 44.1 kHz), through an ADSR envelope (the SPU's: rates and modes as packed in its two ADSR registers) and
 * left / right volumes (0x1000: full). Reverb is accepted, not applied. */

#define SAS_VOICES 32

typedef struct {
    uint32_t vag, size;             /* the ADPCM data */
    int loop;                       /* honour the data's loop flags */
    int playing;
    uint32_t pitch;
    int vol_l, vol_r;
    uint32_t env1, env2;            /* ADSR, as the SPU packs it */
    int phase;                      /* 0 attack, 1 decay, 2 sustain, 3 release */
    float level;                    /* envelope, 0-1 */
    /* decoding */
    uint32_t block;                 /* the next block */
    uint32_t loop_block;
    int16_t samples[28];
    int fill;                       /* samples decoded in samples[] */
    int32_t s1, s2;                 /* the ADPCM filter's history */
    uint32_t frac;                  /* position in samples[], 12.12 fixed */
    int16_t prev;                   /* the sample before samples[0], for interpolation */
    int last_block;                 /* the block just decoded ended the sample */
} Voice;

static Voice voices[SAS_VOICES];

static int decode_block(Voice *v) {
    static const int f0[5] = {0, 60, 115, 98, 122}, f1[5] = {0, 0, -52, -55, -60};
    if (v->last_block)
        return 0;
    if ((v->block + 1) * 16 > v->size)
        return 0;
    const uint8_t *b = PTR(v->vag + v->block * 16);
    int shift = b[0] & 15, filter = (b[0] >> 4) & 7, flags = b[1];
    if (filter > 4)
        filter = 4;
    if (flags & 4)
        v->loop_block = v->block;
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int32_t s = (int16_t)(nib << 12) >> shift;
        s += (v->s1 * f0[filter] + v->s2 * f1[filter] + 32) / 64;
        s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        v->s2 = v->s1;
        v->s1 = s;
        v->samples[i] = (int16_t)s;
    }
    v->fill = 28;
    v->block++;
    if (flags & 1) {                /* the sample's end: loop back, or stop after this block */
        if ((flags & 2) && v->loop)
            v->block = v->loop_block;
        else
            v->last_block = 1;
    }
    return 1;
}

/* One step of the envelope at an SPU rate (0-127): how much the level moves in a sample */
static float env_step(int rate, int decreasing, int exponential, float level) {
    int shift = rate >> 2;
    int step = decreasing ? -8 + (rate & 3) : 7 - (rate & 3);
    float cycles = (float)(1 << (shift > 11 ? shift - 11 : 0));
    float s = (float)(step << (shift < 11 ? 11 - shift : 0));
    if (exponential && !decreasing && level > 0.75f)
        cycles *= 4;
    if (exponential && decreasing)
        s *= level;
    return s / cycles / 32768.0f;
}

static void envelope(Voice *v) {
    uint32_t e1 = v->env1, e2 = v->env2;
    switch (v->phase) {
    case 0:
        v->level += env_step((int)((e1 >> 8) & 0x7F), 0, (e1 >> 15) & 1, v->level);
        if (v->level >= 1.0f) {
            v->level = 1.0f;
            v->phase = 1;
        }
        break;
    case 1: {
        float sustain = (float)((e1 & 15) + 1) / 16.0f;
        v->level += env_step((int)((e1 >> 4) & 15) * 4, 1, 1, v->level);
        if (v->level <= sustain) {
            v->level = sustain;
            v->phase = 2;
        }
        break;
    }
    case 2:
        v->level += env_step((int)((e2 >> 6) & 0x7F), (e2 >> 14) & 1, (e2 >> 15) & 1, v->level);
        break;
    default:
        v->level += env_step((int)(e2 & 31) * 4, 1, (e2 >> 5) & 1, v->level);
        break;
    }
    if (v->level > 1.0f)
        v->level = 1.0f;
    if (v->level <= 0.0f) {
        v->level = 0.0f;
        if (v->phase == 3)
            v->playing = 0;
    }
}

static uint32_t sas_grain = 256;

/* (core, grain, voices, output mode, sample rate) */
void hle___sceSasInit(Ctx *c) {
    sas_grain = ARG1 > 2048 ? 2048 : ARG1;
    memset(voices, 0, sizeof voices);
    RET(0);
}

static Voice *voice(Ctx *c) { return ARG1 < SAS_VOICES ? &voices[ARG1] : NULL; }

/* (core, voice, VAG data, size, loop) */
void hle___sceSasSetVoice(Ctx *c) {
    Voice *v = voice(c);
    if (v) {
        v->vag = ARG2;
        v->size = ARG3;
        v->loop = (int)ARG4;
    }
    RET(v ? 0 : 0x80420010u);
}

void hle___sceSasSetPitch(Ctx *c) {
    Voice *v = voice(c);
    if (v)
        v->pitch = ARG2;
    RET(0);
}

void hle___sceSasSetVolume(Ctx *c) {
    Voice *v = voice(c);
    if (v) {
        v->vol_l = (int)(int32_t)ARG2;
        v->vol_r = (int)(int32_t)ARG3;
    }
    RET(0);
}

void hle___sceSasSetSimpleADSR(Ctx *c) {
    Voice *v = voice(c);
    if (v) {
        v->env1 = ARG2 & 0xFFFF;
        v->env2 = ARG3 & 0xFFFF;
    }
    RET(0);
}

void hle___sceSasSetKeyOn(Ctx *c) {
    Voice *v = voice(c);
    if (v && v->vag) {
        v->playing = 1;
        v->phase = 0;
        v->level = 0;
        v->block = v->loop_block = 0;
        v->s1 = v->s2 = 0;
        v->fill = 0;
        v->frac = 0;
        v->prev = 0;
        v->last_block = 0;
    }
    RET(0);
}

void hle___sceSasSetKeyOff(Ctx *c) {
    Voice *v = voice(c);
    if (v && v->playing)
        v->phase = 3;
    RET(0);
}

/* Which voices have stopped (all ended voices' bits set) */
void hle___sceSasGetEndFlag(Ctx *c) {
    uint32_t f = 0;
    for (int i = 0; i < SAS_VOICES; i++)
        if (!voices[i].playing)
            f |= 1u << i;
    RET(f);
}

void hle___sceSasGetOutputmode(Ctx *c) { RET(0); }
void hle___sceSasRevType(Ctx *c) { RET(0); }
void hle___sceSasRevParam(Ctx *c) { RET(0); }
void hle___sceSasRevEVOL(Ctx *c) { RET(0); }
void hle___sceSasRevVON(Ctx *c) { RET(0); }

/* (core, output): mix one grain of all the voices, 16-bit stereo */
void hle___sceSasCore(Ctx *c) {
    static int32_t mix[2048 * 2];
    uint32_t n = sas_grain;
    memset(mix, 0, sizeof(int32_t) * 2 * n);
    for (int k = 0; k < SAS_VOICES; k++) {
        Voice *v = &voices[k];
        if (!v->playing)
            continue;
        uint32_t pitch = v->pitch ? v->pitch : 0x1000;
        for (uint32_t i = 0; i < n && v->playing; i++) {
            /* advance to the sample the position is in, decoding as needed */
            while ((v->frac >> 12) >= (uint32_t)v->fill) {
                v->prev = v->fill ? v->samples[v->fill - 1] : v->prev;
                v->frac -= (uint32_t)v->fill << 12;
                if (!decode_block(v)) {
                    v->playing = 0;
                    break;
                }
            }
            if (!v->playing)
                break;
            uint32_t idx = v->frac >> 12;
            float t = (float)(v->frac & 0xFFF) / 4096.0f;
            float a = idx ? v->samples[idx - 1] : v->prev, b = v->samples[idx];
            float s = (a + (b - a) * t) * v->level;
            envelope(v);
            mix[2 * i] += (int32_t)(s * (float)v->vol_l / 4096.0f);
            mix[2 * i + 1] += (int32_t)(s * (float)v->vol_r / 4096.0f);
            v->frac += pitch;
        }
    }
    int16_t *out = PTR(ARG1);
    for (uint32_t i = 0; i < 2 * n; i++)
        out[i] = (int16_t)(mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i]);
    RET(0);
}
