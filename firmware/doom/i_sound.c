//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2026 FM-1 Doom contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Sound effects mixer and the music interface.
//
//	The sound lumps of a DPK image (tools/wadpack.py convert_sound):
//	  u16 codec (0: unsigned 8-bit PCM, 2: 4-bit IMA ADPCM, low nibble
//	  first) u16 sample rate, u32 samples, then the data.
//	They play in place from flash. The platform pulls the mix with
//	I_MixSound(), on the FM-1 from the audio interrupt: the engine's
//	calls below only ever switch a channel off before changing it and
//	on again after, so the mixer sees a channel whole or not at all.
//

#include <string.h>

#include "doomtype.h"
#include "doomgeneric.h"
#include "i_music.h"
#include "i_sound.h"
#include "w_wad.h"

#define NUM_CHANNELS 8

typedef struct
{
    const byte *data;
    uint32_t length;            // samples
    uint32_t pos;               // 16.16, in source samples
    uint32_t step;              // 16.16 per output frame (0: set at the first mix)
    uint32_t rate;
    int gain_l, gain_r;         // 0..256
    uint8_t adpcm;
    // ADPCM state: the sample decoded last (index dec_n - 1) and the one before
    int32_t pred;
    int index;
    uint32_t dec_n;
    int16_t prev, cur;
    volatile uint8_t active;
} channel_t;

static channel_t channels[NUM_CHANNELS];

static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767 };
static const int8_t ima_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

#define BARRIER() __asm__ volatile("" ::: "memory")

static int16_t adpcm_next(channel_t *c)
{
    int code = c->data[c->dec_n >> 1];
    int step = ima_step[c->index];
    int32_t diff = step >> 3;

    if (c->dec_n & 1)
        code >>= 4;
    code &= 15;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    if (code & 8)
        c->pred -= diff;
    else
        c->pred += diff;
    if (c->pred > 32767) c->pred = 32767;
    if (c->pred < -32768) c->pred = -32768;
    c->index += ima_index[code & 7];
    if (c->index < 0) c->index = 0;
    if (c->index > 88) c->index = 88;
    c->dec_n++;
    return (int16_t) c->pred;
}

// One channel into the mix, linear interpolation between source samples
static void mix_channel(channel_t *c, int32_t *acc, int frames, int rate)
{
    int i;

    if (!c->step)
        c->step = (uint32_t) (((uint64_t) c->rate << 16) / (uint32_t) rate);
    for (i = 0; i < frames; i++)
    {
        uint32_t n = c->pos >> 16;
        int32_t s;

        if (n + 1 >= c->length)
        {
            c->active = 0;
            return;
        }
        if (c->adpcm)
        {
            // decode up to sample n + 1: cur = sample n + 1, prev = sample n
            while (c->dec_n <= n + 1)
            {
                c->prev = c->cur;
                c->cur = adpcm_next(c);
            }
            s = c->prev + (((c->cur - c->prev) * (int32_t) ((c->pos >> 1) & 0x7FFF)) >> 15);
        }
        else
        {
            int32_t a = ((int32_t) c->data[n] - 128) << 8;
            int32_t b = ((int32_t) c->data[n + 1] - 128) << 8;

            s = a + (((b - a) * (int32_t) ((c->pos >> 1) & 0x7FFF)) >> 15);
        }
        acc[2 * i] += (s * c->gain_l) >> 8;
        acc[2 * i + 1] += (s * c->gain_r) >> 8;
        c->pos += c->step;
    }
}

void I_MixSound(int16_t *stereo, int frames, int rate)
{
    static int32_t acc[2 * 64];
    int i, k, done;

    for (done = 0; done < frames; done += k)
    {
        k = frames - done > 64 ? 64 : frames - done;
        memset(acc, 0, sizeof(acc[0]) * 2 * k);
        for (i = 0; i < NUM_CHANNELS; i++)
        {
            if (channels[i].active)
                mix_channel(&channels[i], acc, k, rate);
        }
        I_MusicMix(acc, k, rate);
        for (i = 0; i < 2 * k; i++)
        {
            int32_t v = acc[i];

            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            stereo[2 * done + i] = (int16_t) v;
        }
    }
}

// vol 0..127, sep 0 (left) .. 254 (right)
static void set_params(channel_t *c, int vol, int sep)
{
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    c->gain_l = ((254 - sep) * vol) >> 6;
    c->gain_r = (sep * vol) >> 6;
}

void I_InitSound(boolean use_sfx_prefix)
{
    memset(channels, 0, sizeof(channels));
}

void I_ShutdownSound(void)
{
    int i;

    for (i = 0; i < NUM_CHANNELS; i++)
        channels[i].active = 0;
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    // the image leaves out the sounds its maps cannot make: -1, silence
    namebuf[0] = 'd';
    namebuf[1] = 's';
    memcpy(namebuf + 2, sfx->name, 6);
    namebuf[8] = '\0';
    return W_CheckNumForName(namebuf);
}

void I_UpdateSound(void)
{
}

void I_UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NUM_CHANNELS)
        return;
    set_params(&channels[channel], vol, sep);
}

int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    channel_t *c;
    const lumpinfo_t *l;
    const byte *d;

    if (channel < 0 || channel >= NUM_CHANNELS)
        return -1;
    c = &channels[channel];
    c->active = 0;
    BARRIER();
    if (sfxinfo->lumpnum < 0)
        return -1;
    l = &lumpinfo[sfxinfo->lumpnum];
    d = W_LumpData(sfxinfo->lumpnum);
    if (l->csize < 8)
        return -1;
    c->adpcm = d[0] == 2;
    c->rate = d[2] | (d[3] << 8);
    c->length = d[4] | (d[5] << 8) | ((uint32_t) d[6] << 16) | ((uint32_t) d[7] << 24);
    c->data = d + 8;
    c->pos = 0;
    c->step = 0;
    c->pred = 0;
    c->index = 0;
    c->dec_n = 0;
    c->prev = c->cur = 0;
    if (c->adpcm)
        c->cur = adpcm_next(c);         // sample 0
    set_params(c, vol, sep);
    BARRIER();
    c->active = 1;
    return channel;
}

void I_StopSound(int channel)
{
    if (channel >= 0 && channel < NUM_CHANNELS)
        channels[channel].active = 0;
}

boolean I_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS)
        return false;
    return channels[channel].active != 0;
}

void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
}

void I_BindSoundVariables(void)
{
}
