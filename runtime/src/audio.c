/* Audio: the output channels, and the SAS (the PSP's software synthesiser the game's sound engine drives).
 *
 * Output is timed as the hardware plays it - a channel takes one buffer at a time, and outputting the next blocks until
 * the one before has played, at 44.1 kHz - but not heard yet: nothing goes to the host's audio. The SAS mixes silence:
 * its voices (VAG ADPCM, ADSR envelopes, pitch) are still to do. */
#include "hle.h"

#include <string.h>

#define CHANNELS 8
#define RATE 44100

static struct { int reserved; uint32_t samples, format, vol_l, vol_r; uint64_t queued_until; } chan[CHANNELS];

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
    RET(chan[ch].samples);
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
