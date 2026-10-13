/* Audio: the output channels, and the SAS (the PSP's software synthesiser the game's sound engine drives).
 *
 * Output is timed as the hardware plays it - a channel takes one buffer at a time, and outputting the next blocks until
 * the one before has played, at 44.1 kHz - and heard through SDL: each channel a stream on the default device, which
 * mixes them (none in headless runs). The SAS mixes silence: its voices (VAG ADPCM, ADSR envelopes, pitch) are still
 * to do - the game plays its sound through ATRAC (atrac.c) as far as it has been run. */
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
static void play(int ch, uint32_t addr, uint32_t vol_l, uint32_t vol_r) {
    dump(ch, addr);
    if (!audio_on())
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

static uint32_t sas_grain = 256, sas_voices = 32;

/* (core, grain, voices, output mode, sample rate) */
void hle___sceSasInit(Ctx *c) {
    sas_grain = ARG1;
    sas_voices = ARG2;
    RET(0);
}

/* (core, output): one grain of stereo 16-bit samples */
void hle___sceSasCore(Ctx *c) {
    memset(PTR(ARG1), 0, sas_grain * 4);
    RET(0);
}

/* Every voice has ended (none plays) */
void hle___sceSasGetEndFlag(Ctx *c) {
    RET(sas_voices >= 32 ? 0xFFFFFFFFu : (1u << sas_voices) - 1);
}
