// SPDX-License-Identifier: GPL-2.0-or-later
//
// Music: the MUS lumps played by a small FM synthesizer after the OPL2 (the
// AdLib / Sound Blaster chip Doom's music was written for), with the
// instruments of the GENMIDI lump. Fitting, on a synthesizer.
//
// Each voice is two operators: a modulator, which may feed back into
// itself, and a carrier, either phase-modulated by it (FM) or added to it.
// Operators have the OPL2's four waveforms, frequency multipliers, total
// level and an ADSR envelope with the OPL2's rates; amplitudes are worked
// in its 0.1875 dB attenuation steps. Not an emulation: no key scaling, no
// tremolo or vibrato, no OPL rate tables to the cycle.
//
// I_MusicMix() runs in the audio interrupt on the FM-1. The engine's calls
// (I_PlaySong, I_StopSong ...) only set requests the mixer takes up at its
// next block, so the two never work on a song at the same time.

#include <string.h>

#include "doomtype.h"
#include "i_music.h"
#include "i_sound.h"
#include "i_swap.h"
#include "w_wad.h"
#include "z_zone.h"

int snd_sfxdevice = SNDDEVICE_SB;
int snd_musicdevice = SNDDEVICE_GENMIDI;
int snd_samplerate = 44100;
int snd_cachesize = 0;
int snd_maxslicetime_ms = 28;
char *snd_musiccmd = "";

#define NUM_VOICES 12
#define MUS_RATE 140            // MUS ticks per second
#define BLOCK 16                // samples between envelope steps
#define ATT_MAX 511             // 96 dB in 0.1875 dB steps: silence

// ------------------------------------------------------------ tables

static int16_t sine[1024];      // one cycle, +-4095
static uint16_t att_gain[512];  // attenuation step -> linear gain, Q12
static uint32_t note_inc[128];  // MIDI note -> phase step at the mix rate
static int table_rate;

// one OPL2 GENMIDI voice: modulator and carrier registers, feedback/connection
typedef struct
{
    uint8_t m_char, m_attack, m_sustain, m_wave, m_ksl, m_level;
    uint8_t feedback;
    uint8_t c_char, c_attack, c_sustain, c_wave, c_ksl, c_level;
    uint8_t unused;
    int16_t base_note;
} PACKEDATTR genmidi_voice_t;

typedef struct
{
    uint16_t flags;             // 1 fixed pitch, 4 two voices
    uint8_t fine_tune;
    uint8_t fixed_note;
    genmidi_voice_t voice[2];
} PACKEDATTR genmidi_instr_t;

#define GENMIDI_INSTRS 175

static const genmidi_instr_t *instruments;

static const uint8_t mult_x2[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };

// envelope times (ms): attack to full, and decay/release over 96 dB (OPL2 data sheet)
static const uint16_t attack_ms[16] = { 0, 2826, 1413, 706, 353, 176, 88, 44, 22, 11, 6, 3, 2, 1, 1, 0 };
static const uint32_t decay_ms[16] = { 0, 39280, 19640, 9820, 4910, 2455, 1227, 614, 307, 153, 77, 38, 19, 10, 5,
                                       2 };

static void init_tables(int rate)
{
    int i;

    // sine by the rotation recurrence s[n+1] = 2 cos(w) s[n] - s[n-1], w = 2 pi / 1024, Q30
    {
        const int64_t c2 = 2147443222;  // 2 cos(2 pi / 1024) in Q30
        int64_t s0 = 0, s1 = 6588356;   // sin(0), sin(2 pi / 1024) in Q30

        for (i = 0; i < 1024; i++)
        {
            int64_t s2;

            sine[i] = (int16_t) ((s0 * 4095 + (1 << 29)) >> 30);
            s2 = ((c2 * s1) >> 30) - s0;
            s0 = s1;
            s1 = s2;
        }
    }
    // attenuation (0.1875 dB steps) -> gain: 10^(-0.1875 n / 20), by repeated multiplication in Q30
    {
        int64_t g = (int64_t) 1 << 30;
        const int64_t step = 1050811628;        // 10^(-0.1875/20) in Q30

        for (i = 0; i < 512; i++)
        {
            att_gain[i] = (uint16_t) ((g + (1 << 17)) >> 18);
            g = (g * step) >> 30;
        }
        att_gain[511] = 0;
    }
    // note -> phase step: 2^32 * f / rate, f = 440 * 2^((n - 69) / 12)
    {
        // the octave from note 60 (C4, 261.6256 Hz), each 2^32 * f / 1 Hz scaled down later
        static const uint32_t semitone_q16[12] = {
            65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715 };

        for (i = 0; i < 128; i++)
        {
            int oct = i / 12 - 5, k = i % 12;
            // f(n) = 261.6256 * semitone[k] / 65536 * 2^oct ; inc = f * 2^32 / rate
            uint64_t f_q16 = (uint64_t) 17145893 * semitone_q16[k] >> 16;     // 261.6256 Hz in Q16 * ratio
            uint64_t inc;

            if (oct >= 0)
                f_q16 <<= oct;
            else
                f_q16 >>= -oct;
            inc = (f_q16 << 16) / (uint64_t) rate;
            note_inc[i] = (uint32_t) inc;
        }
    }
    table_rate = rate;
}

// ------------------------------------------------------------ operators, voices

typedef struct
{
    uint32_t phase, inc;
    uint8_t wave, stage;        // stage: 0 off, 1 attack, 2 decay, 3 sustain, 4 release
    uint8_t sustained;          // EG type: holds at the sustain level until key off
    int32_t env;                // attenuation, 0.1875 dB steps << 16
    int32_t att_rate, dec_rate, rel_rate;   // per block, << 16
    int32_t sus_level;          // << 16
    int level;                  // total level + note volume, 0.1875 dB steps
    int out, prev;              // last outputs (feedback)
    int gain;                   // Q12, from env + level, updated per block
} op_t;

typedef struct
{
    op_t mod, car;
    uint8_t active, channel, key, note, additive, feedback, voice_index;   // key: the MUS note it plays
    int note_volume;            // attenuation from velocity and channel volume
    uint32_t age;
    const genmidi_voice_t *gv;
    int base_note, fine;
    int bend;                   // the channel's, at the time of the last update
} voice_t;

static voice_t voices[NUM_VOICES];
static uint32_t voice_age;

static int16_t wave_out(int w, uint32_t idx)
{
    idx &= 1023;
    switch (w & 3)
    {
      case 0:
        return sine[idx];
      case 1:                   // half sine
        return idx < 512 ? sine[idx] : 0;
      case 2:                   // absolute sine
        return sine[idx & 511];
      default:                  // quarter sine pulses
        return (idx & 256) ? 0 : sine[idx & 255];
    }
}

static int32_t rate_per_block(int ms_96db)
{
    // steps of 1/65536 of an attenuation step per block to cover 96 dB (512 steps) in ms_96db
    int64_t blocks;

    if (ms_96db <= 0)
        return 512 << 16;
    blocks = (int64_t) ms_96db * table_rate / (1000 * BLOCK);
    if (blocks < 1)
        blocks = 1;
    return (int32_t) (((int64_t) 512 << 16) / blocks);
}

static void op_setup(op_t *o, uint8_t chr, uint8_t attack, uint8_t sustain, uint8_t wave, uint8_t level)
{
    o->wave = wave & 3;
    o->sustained = (chr & 0x20) != 0;
    o->att_rate = attack >> 4 ? rate_per_block(attack_ms[attack >> 4]) : 0;
    o->dec_rate = (attack & 15) ? rate_per_block(decay_ms[attack & 15]) : 0;
    o->rel_rate = (sustain & 15) ? rate_per_block(decay_ms[sustain & 15]) : 0;
    o->sus_level = ((sustain >> 4) == 15 ? ATT_MAX : (sustain >> 4) * 16) << 16;    // 3 dB = 16 steps
    o->level = (level & 63) * 4;                                                     // 0.75 dB = 4 steps
}

static uint32_t op_inc(int note, int fine, int bend, uint8_t chr)
{
    // note + bend (1/64 semitones) -> phase step, then the multiplier
    int pos = note * 64 + bend + fine;
    int n, f;
    uint32_t a, b, inc;

    if (pos < 0)
        pos = 0;
    if (pos > 127 * 64)
        pos = 127 * 64;
    n = pos >> 6;
    f = pos & 63;
    a = note_inc[n];
    b = n < 127 ? note_inc[n + 1] : a;
    inc = a + (uint32_t) (((uint64_t) (b - a) * (uint32_t) f) >> 6);
    return (uint32_t) (((uint64_t) inc * mult_x2[chr & 15]) >> 1);
}

static void op_step(op_t *o)    // the envelope, once a block
{
    int att;

    switch (o->stage)
    {
      case 1:
        if (o->att_rate == 0)
            break;
        if (o->att_rate >= (512 << 16))
            o->env = 0;
        else    // the OPL2's attack: the attenuation falls exponentially (fast at first), to 0 in the attack time
            o->env -= (int32_t) (((int64_t) o->env * 6 * o->att_rate) / ((int64_t) 512 << 16)) + (1 << 12);
        if (o->env <= 0)
        {
            o->env = 0;
            o->stage = 2;
        }
        break;
      case 2:
        o->env += o->dec_rate;
        if (o->env >= o->sus_level)
        {
            o->env = o->sus_level;
            o->stage = 3;
        }
        break;
      case 3:
        if (!o->sustained)
            o->env += o->rel_rate;
        break;
      case 4:
        o->env += o->rel_rate ? o->rel_rate : (8 << 16);
        break;
    }
    if (o->env > (ATT_MAX << 16))
    {
        o->env = ATT_MAX << 16;
        if (o->stage >= 3)
            o->stage = 0;
    }
    att = (o->env >> 16) + o->level;
    o->gain = o->stage == 0 || att >= ATT_MAX ? 0 : att_gain[att];
}

// ------------------------------------------------------------ the MUS player

typedef struct
{
    uint8_t instrument, volume, pan;
    int bend;                   // 1/64 semitones
} channel_t;

static channel_t channels[16];

static const byte *score, *score_end, *pos;
static int song_looping;
static uint32_t wait_samples;   // until the next event, in samples << 8
static uint32_t samples_per_tick;   // << 8

// requests from the engine, taken by the mixer
static const byte *volatile req_song;
static volatile int req_play, req_stop, req_looping, req_pause;
static volatile int music_volume = 100;    // 0..127
static int paused;
static int32_t dc_x, dc_y;      // the DC blocker's state
static volatile int playing;

// velocity / volume 0..127 -> attenuation steps (20 log10)
static int volume_att(int v)
{
    // 127 -> 0; each halving -> 6 dB = 32 steps
    int att = 0;

    if (v <= 0)
        return ATT_MAX;
    while (v < 64)
    {
        v <<= 1;
        att += 32;
    }
    // 64..127: roughly linear in dB over the last 6 dB
    return att + (127 - v) * 32 / 64;
}

static void voice_update_pitch(voice_t *v)
{
    channel_t *ch = &channels[v->channel];
    int note = v->note + v->base_note;

    v->mod.inc = op_inc(note, v->fine, ch->bend, v->gv->m_char);
    v->car.inc = op_inc(note, v->fine, ch->bend, v->gv->c_char);
}

static void voice_update_level(voice_t *v)
{
    channel_t *ch = &channels[v->channel];
    int vol = volume_att(v->note_volume) + volume_att(ch->volume) + volume_att(music_volume);

    v->car.level = (v->gv->c_level & 63) * 4 + vol;
    if (v->additive)
        v->mod.level = (v->gv->m_level & 63) * 4 + vol;
}

static voice_t *voice_alloc(void)
{
    voice_t *best = &voices[0];
    int i;

    for (i = 0; i < NUM_VOICES; i++)
    {
        if (!voices[i].active)
            return &voices[i];
        // steal the oldest released voice, else the oldest
        if ((voices[i].car.stage == 4) > (best->car.stage == 4)
         || ((voices[i].car.stage == 4) == (best->car.stage == 4) && voices[i].age < best->age))
            best = &voices[i];
    }
    return best;
}

static void note_on_voice(int chan, int note, int velocity, const genmidi_instr_t *in, int vi)
{
    const genmidi_voice_t *gv = &in->voice[vi];
    voice_t *v = voice_alloc();

    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->channel = (uint8_t) chan;
    v->key = (uint8_t) note;
    v->note = (uint8_t) ((SHORT(in->flags) & 1) ? in->fixed_note : note);
    v->voice_index = (uint8_t) vi;
    v->gv = gv;
    v->base_note = SHORT(gv->base_note);
    v->fine = vi ? ((int) in->fine_tune - 128) / 2 : 0;    // the second voice detuned
    v->additive = gv->feedback & 1;
    v->feedback = (gv->feedback >> 1) & 7;
    v->note_volume = velocity;
    v->age = ++voice_age;
    op_setup(&v->mod, gv->m_char, gv->m_attack, gv->m_sustain, gv->m_wave, gv->m_level);
    op_setup(&v->car, gv->c_char, gv->c_attack, gv->c_sustain, gv->c_wave, gv->c_level);
    v->mod.stage = v->car.stage = 1;
    v->mod.env = v->car.env = ATT_MAX << 16;
    voice_update_pitch(v);
    voice_update_level(v);
}

static void note_on(int chan, int note, int velocity)
{
    const genmidi_instr_t *in;

    if (instruments == NULL)
        return;
    if (chan == 15)             // percussion: an instrument per note
    {
        if (note < 35 || note > 81)
            return;
        in = &instruments[128 + note - 35];
    }
    else
        in = &instruments[channels[chan].instrument];
    note_on_voice(chan, note, velocity, in, 0);
    if (SHORT(in->flags) & 4)
        note_on_voice(chan, note, velocity, in, 1);
}

static void note_off(int chan, int note)
{
    int i;

    for (i = 0; i < NUM_VOICES; i++)
    {
        voice_t *v = &voices[i];

        if (v->active && v->channel == chan && v->key == note && v->car.stage != 4)
            v->mod.stage = v->car.stage = 4;
    }
}

static void all_notes_off(int chan, int hard)
{
    int i;

    for (i = 0; i < NUM_VOICES; i++)
        if (voices[i].active && (chan < 0 || voices[i].channel == chan))
        {
            if (hard)
                voices[i].active = 0;
            else
                voices[i].mod.stage = voices[i].car.stage = 4;
        }
}

static void reset_channels(void)
{
    int i;

    for (i = 0; i < 16; i++)
    {
        channels[i].instrument = 0;
        channels[i].volume = 100;
        channels[i].pan = 64;
        channels[i].bend = 0;
    }
}

static void channel_changed(int chan, int pitch)
{
    int i;

    for (i = 0; i < NUM_VOICES; i++)
        if (voices[i].active && voices[i].channel == chan)
        {
            if (pitch)
                voice_update_pitch(&voices[i]);
            else
                voice_update_level(&voices[i]);
        }
}

// one MUS event; returns 0 at the end of the score
static int mus_event(void)
{
    byte ev, chan;
    int last;

    if (pos >= score_end)
        return 0;
    ev = *pos++;
    last = ev & 0x80;
    chan = ev & 15;
    switch ((ev >> 4) & 7)
    {
      case 0:                   // release note
        note_off(chan, *pos++ & 127);
        break;
      case 1:                   // play note
      {
        byte n = *pos++;
        static uint8_t last_volume[16] = { 100, 100, 100, 100, 100, 100, 100, 100,
                                           100, 100, 100, 100, 100, 100, 100, 100 };

        if (n & 0x80)
            last_volume[chan] = *pos++ & 127;
        note_on(chan, n & 127, last_volume[chan]);
        break;
      }
      case 2:                   // pitch wheel: 0..255, 128 = none, +-1 tone
        channels[chan].bend = ((int) *pos++ - 128) * 64 * 2 / 128;
        channel_changed(chan, 1);
        break;
      case 3:                   // system event
        switch (*pos++)
        {
          case 10:              // all sounds off
            all_notes_off(chan, 1);
            break;
          case 11:              // all notes off
            all_notes_off(chan, 0);
            break;
          case 14:              // reset all controllers
            channels[chan].bend = 0;
            channels[chan].volume = 100;
            break;
        }
        break;
      case 4:                   // controller
      {
        byte c = *pos++, val = *pos++ & 127;

        if (c == 0)
            channels[chan].instrument = val;
        else if (c == 3)
        {
            channels[chan].volume = val;
            channel_changed(chan, 0);
        }
        else if (c == 4)
            channels[chan].pan = val;
        break;
      }
      case 5:                   // end of measure
        break;
      case 6:                   // score end
        return 0;
      default:
        break;
    }
    if (last)
    {
        uint32_t t = 0;
        byte b;

        do
        {
            b = *pos++;
            t = t * 128 + (b & 127);
        } while ((b & 0x80) && pos < score_end);
        wait_samples += t * samples_per_tick;
    }
    return 1;
}

static void song_start(const byte *data, int looping)
{
    uint16_t len = data[4] | (data[5] << 8), start = data[6] | (data[7] << 8);

    all_notes_off(-1, 1);
    reset_channels();
    score = data + start;
    score_end = score + len;
    pos = score;
    song_looping = looping;
    wait_samples = 0;
    playing = 1;
}

// ------------------------------------------------------------ the mix

static void render(voice_t *v, int32_t *out, int frames)
{
    int i;
    op_t *m = &v->mod, *c = &v->car;
    int32_t mg = m->gain, cg = c->gain;

    for (i = 0; i < frames; i++)
    {
        int fbmod = v->feedback ? (m->prev + m->out) >> (9 - v->feedback) : 0;
        int mo, co, s;

        mo = (wave_out(m->wave, (m->phase >> 22) + (uint32_t) fbmod) * mg) >> 12;
        m->prev = m->out;
        m->out = mo;
        m->phase += m->inc;
        if (v->additive)
            co = (wave_out(c->wave, c->phase >> 22) * cg) >> 12;
        else
            co = (wave_out(c->wave, (c->phase >> 22) + (uint32_t) mo) * cg) >> 12;
        c->phase += c->inc;
        s = v->additive ? mo + co : co * 2;     // (x2: the music sits ~12 dB under a full sound)
        out[i] += s;
    }
}

void I_MusicMix(int32_t *acc, int frames, int rate)
{
    int done = 0, i;

    if (rate != table_rate)
        init_tables(rate);
    if (req_stop)
    {
        req_stop = 0;
        playing = 0;
        all_notes_off(-1, 1);
    }
    if (req_play)
    {
        req_play = 0;
        if (req_song != NULL)
            song_start(req_song, req_looping);
    }
    paused = req_pause;
    samples_per_tick = (uint32_t) (((uint64_t) rate << 8) / MUS_RATE);
    if (paused)
        return;

    while (done < frames)
    {
        int n = frames - done > BLOCK ? BLOCK : frames - done;

        // the events due by this block
        while (playing && wait_samples < (uint32_t) (n << 8))
        {
            if (!mus_event())
            {
                if (song_looping)
                {
                    pos = score;
                    all_notes_off(-1, 0);
                }
                else
                {
                    playing = 0;
                    all_notes_off(-1, 0);
                }
                break;
            }
        }
        if (playing)
            wait_samples -= (uint32_t) n << 8;

        {
            int32_t mono[BLOCK];

            memset(mono, 0, sizeof(mono));
            for (i = 0; i < NUM_VOICES; i++)
            {
                voice_t *v = &voices[i];

                if (!v->active)
                    continue;
                op_step(&v->mod);
                op_step(&v->car);
                if (v->car.stage == 0 && (!v->additive || v->mod.stage == 0))
                {
                    v->active = 0;
                    continue;
                }
                render(v, mono, n);
            }
            // a DC blocker (~30 Hz): the half and quarter sine waves are all one side of zero
            for (i = 0; i < n; i++)
            {
                dc_y = mono[i] - dc_x + ((dc_y * 32613) >> 15);
                dc_x = mono[i];
                acc[2 * (done + i)] += dc_y;
                acc[2 * (done + i) + 1] += dc_y;
            }
        }
        done += n;
    }
}

// ------------------------------------------------------------ the engine's interface

void I_InitMusic(void)
{
    int lump = W_CheckNumForName("GENMIDI");

    // GENMIDI: "#OPL_II#" and 175 instruments (the image keeps it raw: read in place)
    instruments = NULL;
    if (lump >= 0 && W_LumpLength(lump) >= 8 + GENMIDI_INSTRS * (int) sizeof(genmidi_instr_t))
    {
        const byte *g = W_CacheLumpNum(lump, PU_STATIC);

        if (!memcmp(g, "#OPL_II#", 8))
            instruments = (const genmidi_instr_t *) (g + 8);
    }
    reset_channels();
}

void I_ShutdownMusic(void)
{
    req_stop = 1;
}

void I_SetMusicVolume(int volume)
{
    int i;

    music_volume = volume;
    for (i = 0; i < NUM_VOICES; i++)
        if (voices[i].active)
            voice_update_level(&voices[i]);
}

void I_PauseSong(void)
{
    req_pause = 1;
}

void I_ResumeSong(void)
{
    req_pause = 0;
}

void *I_RegisterSong(void *data, int len)
{
    const byte *d = data;

    if (len < 16 || memcmp(d, "MUS\x1a", 4) != 0)
        return NULL;
    return data;
}

void I_UnRegisterSong(void *handle)
{
}

void I_PlaySong(void *handle, boolean looping)
{
    if (handle == NULL || instruments == NULL)
        return;
    req_song = handle;
    req_looping = looping;
    req_stop = 0;
    req_play = 1;
}

void I_StopSong(void)
{
    req_play = 0;
    req_stop = 1;
}

boolean I_MusicIsPlaying(void)
{
    return playing || req_play;
}
