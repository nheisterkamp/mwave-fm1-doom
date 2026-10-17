/* SPDX-License-Identifier: GPL-3.0-only */
/* The music synthesizer (firmware/doom/i_music.c) alone: notes of the IWAD's GENMIDI instruments, their pitch
 * measured. tests/synth_test GENMIDI.lmp: a line per note; exit 1 if one is off.
 * A note's waveform repeats at its frequency times the greatest common divisor of its operators' multipliers:
 * with a modulator at x0.5 (most of GENMIDI, as on the OPL2) that is half the note's frequency. The pitch found
 * (autocorrelation) is either. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* what i_music.c takes from the engine */
#include "doomtype.h"
#include "w_wad.h"
const lumpinfo_t *lumpinfo;
unsigned int numlumps;
int W_CheckNumForName(char *name) { (void)name; return -1; }
int W_LumpLength(unsigned int lump) { (void)lump; return 0; }
void *W_CacheLumpNum(int lump, int tag) { (void)lump; (void)tag; return NULL; }
#include "../firmware/doom/i_music.c"

#define RATE 44118
static int32_t acc[2 * RATE];

static double measure(const int32_t *x, int n)        /* autocorrelation pitch of the left channel */
{
    int lag, best = 0;
    double bestv = 0;
    for (lag = 20; lag < 2000; lag++) {
        double v = 0;
        int i;
        for (i = 0; i + lag < n; i += 2)
            v += (double)x[2 * i] * x[2 * (i + lag)];
        if (v > bestv) {
            bestv = v;
            best = lag;
        }
    }
    return best ? (double)RATE / best : 0;
}

int main(int argc, char **argv)
{
    static unsigned char g[20000];
    FILE *f = fopen(argv[1], "rb");
    static const int tests[][2] = {{0, 69}, {0, 57}, {29, 52}, {30, 64}, {33, 40}, {80, 72}, {19, 60}};
    size_t i;
    int ok, bad = 0;
    if (!f || fread(g, 1, sizeof g, f) < 8 + 175 * 36)
        return 2;
    init_tables(RATE);
    instruments = (const genmidi_instr_t *)(g + 8);
    reset_channels();
    for (i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int k, peak = 0;
        double want = 440.0 * pow(2, (tests[i][1] + SHORT(instruments[tests[i][0]].voice[0].base_note) - 69) / 12.0);
        memset(acc, 0, sizeof acc);
        all_notes_off(-1, 1);
        channels[0].instrument = (uint8_t)tests[i][0];
        note_on(0, tests[i][1], 127);
        for (k = 0; k < RATE / 2; k += 64) {
            int j;
            for (j = 0; j < NUM_VOICES; j++)
                if (voices[j].active) {
                    op_step(&voices[j].mod);
                    op_step(&voices[j].car);
                }
            /* render blocks of BLOCK as I_MusicMix does */
            for (j = 0; j < 64; j += BLOCK) {
                int v;
                static int32_t mono[BLOCK];
                int q;
                memset(mono, 0, sizeof mono);
                for (v = 0; v < NUM_VOICES; v++)
                    if (voices[v].active)
                        render(&voices[v], mono, BLOCK);
                for (q = 0; q < BLOCK; q++)
                    acc[2 * (k + j + q)] = acc[2 * (k + j + q) + 1] = mono[q];
            }
        }
        for (k = 0; k < RATE / 2; k++)
            if (abs(acc[2 * k]) > peak)
                peak = abs(acc[2 * k]);
        {
            const genmidi_voice_t *gv = &instruments[tests[i][0]].voice[0];
            int a = mult_x2[gv->m_char & 15], b = mult_x2[gv->c_char & 15];
            double got = measure(acc + RATE / 4 * 2, RATE / 4), rep;
            while (b) { int t = a % b; a = b; b = t; }
            rep = want * a / 2.0;
            ok = fabs(got - want) < want * 0.02 || fabs(got - rep) < rep * 0.02;
            bad += !ok || peak < 1000;
            printf("instrument %3d note %3d: %7.1f Hz (repeats at %7.1f Hz), measured %7.1f Hz, peak %5d  %s\n",
                   tests[i][0], tests[i][1], want, rep, got, peak, ok && peak >= 1000 ? "ok" : "WRONG");
        }
    }
    return bad != 0;
}
