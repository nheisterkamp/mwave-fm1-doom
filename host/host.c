// SPDX-License-Identifier: GPL-3.0-only
/* The engine on a computer, headless, for tests: the platform of doomgeneric.h with a virtual clock (one
 * Doom tic per frame), input from a script, the panel's 240x240 picture (the firmware's own conversion,
 * firmware/fm1/lcd_conv.c) written as PNG, the sound mix as WAV, and the zone limited as on the FM-1.
 *
 *   host/doom [host options] [engine options]
 *     -dpk FILE        the DPK image (default build/doom.dpk)
 *     -zone KB         zone size (default: as the firmware)
 *     -frames N        stop after N frames (0: run until the engine quits)
 *     -shot N[,N..]    write out/frame_NNNNN.png at these frames
 *     -shotevery N     ... and every N frames
 *     -out DIR         where (default build/host)
 *     -script FILE     input: lines "FRAME down|up KEY", "FRAME turn DX", "FRAME fm1 down|up CONTROL"
 *     -wav FILE        write the sound
 *     -stats           print zone and frame statistics at the end
 *   engine options: -warp E M, -skill N, -playdemo LUMP, -timedemo LUMP, -nomonsters ... */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <execinfo.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "fm1_input_map.h"
#include "lcd_conv.h"
#include "z_zone.h"

#ifndef ZONE_KB_DEFAULT
#define ZONE_KB_DEFAULT 1024           /* (64-bit pointers: more than the FM-1 needs; tests/run_tests.sh measures 32-bit) */
#endif

static uint8_t *pack;
static long pack_len;
static uint8_t *zone;
static int zone_kb = ZONE_KB_DEFAULT;
static long frames, max_frames = 0, shot_every = 0;
static long shots[64];
static int nshots;
static const char *out_dir = "build/host";
static uint32_t now_ms;
static FILE *wav;
static long wav_frames;
static int stats;
static uint16_t lcd[LCD_W * LCD_H];

/* ------------------------------------------------------------------ script */

typedef struct { long frame; int kind; int down; int code; } script_ev_t;   /* kind 0 key, 1 turn, 2 fm1 */
static script_ev_t script[4096];
static int nscript, script_pos;

static const struct { const char *name; int key; } KEYNAMES[] = {
    {"up", KEY_UPARROW}, {"down", KEY_DOWNARROW}, {"left", KEY_LEFTARROW}, {"right", KEY_RIGHTARROW},
    {"fire", KEY_FIRE}, {"use", KEY_USE}, {"enter", KEY_ENTER}, {"escape", KEY_ESCAPE}, {"tab", KEY_TAB},
    {"strafel", KEY_STRAFE_L}, {"strafer", KEY_STRAFE_R}, {"run", KEY_RSHIFT}, {"y", 'y'}, {"n", 'n'},
    {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'}, {"5", '5'}, {"6", '6'}, {"7", '7'},
};

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f) {
        fprintf(stderr, "host: %s: cannot open\n", path);
        exit(2);
    }
    while (fgets(line, sizeof line, f) && nscript < (int)(sizeof script / sizeof script[0])) {
        char a[32], b[32], c[32];
        long fr;
        int n = sscanf(line, "%ld %31s %31s %31s", &fr, a, b, c);
        script_ev_t *e = &script[nscript];
        size_t i;
        if (n < 3 || line[0] == '#')
            continue;
        e->frame = fr;
        if (!strcmp(a, "turn")) {
            e->kind = 1;
            e->code = atoi(b);
        } else if (!strcmp(a, "fm1") && n == 4) {
            e->kind = 2;
            e->down = !strcmp(b, "down");
            e->code = fm1_control_by_name(c);
            if (e->code < 0) {
                fprintf(stderr, "host: unknown FM-1 control %s\n", c);
                exit(2);
            }
        } else {
            e->kind = 0;
            e->down = !strcmp(a, "down");
            e->code = -1;
            for (i = 0; i < sizeof KEYNAMES / sizeof KEYNAMES[0]; i++)
                if (!strcmp(b, KEYNAMES[i].name))
                    e->code = KEYNAMES[i].key;
            if (e->code < 0 && strlen(b) == 1)
                e->code = b[0];
            if (e->code < 0) {
                fprintf(stderr, "host: unknown key %s\n", b);
                exit(2);
            }
        }
        nscript++;
    }
    fclose(f);
}

/* ------------------------------------------------------------------ PNG */

static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t b[4] = {n >> 24, n >> 16, n >> 8, n};
    uLong crc = crc32(0, (const Bytef *)type, 4);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n)
        fwrite(d, 1, n, f);
    crc = crc32(crc, d, n);
    b[0] = crc >> 24; b[1] = crc >> 16; b[2] = crc >> 8; b[3] = crc;
    fwrite(b, 1, 4, f);
}

static void write_png(const char *path)
{
    static uint8_t raw[LCD_H * (1 + LCD_W * 3)], z[LCD_H * (1 + LCD_W * 3) + 1024];
    uLongf zn = sizeof z;
    uint8_t ihdr[13] = {0, 0, LCD_W >> 8, LCD_W & 255, 0, 0, LCD_H >> 8, LCD_H & 255, 8, 2, 0, 0, 0};
    FILE *f;
    int x, y;
    for (y = 0; y < LCD_H; y++) {
        uint8_t *r = raw + y * (1 + LCD_W * 3);
        r[0] = 0;
        for (x = 0; x < LCD_W; x++) {
            uint16_t v = lcd[y * LCD_W + x];
            uint16_t c = (uint16_t)((v >> 8) | (v << 8));       /* back from high byte first */
            r[1 + 3 * x] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            r[2 + 3 * x] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            r[3 + 3 * x] = (uint8_t)((c & 31) * 255 / 31);
        }
    }
    compress2(z, &zn, raw, sizeof raw, 6);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "host: cannot write %s\n", path);
        return;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
}

/* ------------------------------------------------------------------ WAV */

static void wav_header(FILE *f, long nframes)
{
    uint32_t rate = 44118, bytes = (uint32_t)nframes * 4u;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    *(uint32_t *)(h + 4) = 36 + bytes;
    memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16;
    *(uint16_t *)(h + 20) = 1;
    *(uint16_t *)(h + 22) = 2;
    *(uint32_t *)(h + 24) = rate;
    *(uint32_t *)(h + 28) = rate * 4;
    *(uint16_t *)(h + 32) = 4;
    *(uint16_t *)(h + 34) = 16;
    memcpy(h + 36, "data", 4);
    *(uint32_t *)(h + 40) = bytes;
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

/* ------------------------------------------------------------------ the platform */

void DG_Init(void) {}

void DG_SetPalette(const uint8_t *rgb) { lcd_conv_palette(rgb); }

static int is_shot(long f)
{
    int i;
    if (shot_every && f % shot_every == 0)
        return 1;
    for (i = 0; i < nshots; i++)
        if (shots[i] == f)
            return 1;
    return 0;
}

static void end_run(int code);

void DG_DrawFrame(const uint8_t *screen)
{
    extern void W_FrameStats(void);
    W_FrameStats();
    frames++;
    now_ms = (uint32_t)(frames * 1000 / 35);
    if (is_shot(frames)) {
        char path[512];
        memset(lcd, 0, sizeof lcd);
        lcd_conv_rows(screen, 0, VIEW_H, lcd + VIEW_Y * LCD_W);
        snprintf(path, sizeof path, "%s/frame_%05ld.png", out_dir, frames);
        write_png(path);
    }
    if (wav) {                                  /* a tic of sound */
        static int16_t buf[2 * 2048];
        long want = (frames * 44118L) / 35 - wav_frames;
        while (want > 0) {
            int k = want > 2048 ? 2048 : (int)want;
            I_MixSound(buf, k, 44118);
            fwrite(buf, 4, k, wav);
            wav_frames += k;
            want -= k;
        }
    }
    if (max_frames && frames >= max_frames)
        end_run(0);
}

uint32_t DG_GetTicksMs(void) { return now_ms; }

void DG_SleepMs(uint32_t ms)
{
    now_ms += ms;
}

int DG_GetEvent(event_t *ev)
{
    while (script_pos < nscript && script[script_pos].frame <= frames) {
        script_ev_t *e = &script[script_pos++];
        if (e->kind == 2) {
            fm1_map_control(e->code, e->down);
            continue;
        }
        memset(ev, 0, sizeof *ev);
        if (e->kind == 1) {
            ev->type = ev_mouse;
            ev->data2 = e->code;
        } else {
            ev->type = e->down ? ev_keydown : ev_keyup;
            ev->data1 = e->code;
            ev->data2 = e->code;
        }
        return 1;
    }
    return fm1_map_event(ev);
}

uint8_t *DG_ZoneBase(int *size)
{
    *size = zone_kb * 1024;
    zone = malloc((size_t)*size);
    return zone;
}

const void *DG_PackPart(int part)
{
    const uint32_t *h = (const uint32_t *)pack;          /* dpk_header_t: end_a, end_b at words 5, 6 */
    uint32_t start = part == 0 ? 0 : h[4 + part];
    return start < (uint32_t)pack_len ? pack + start : NULL;
}

void DG_Log(const char *s) { fprintf(stderr, "%s\n", s); }

void DG_Error(const char *msg)
{
    fprintf(stderr, "ERROR at frame %ld: %s\n", frames, msg);
    if (getenv("ZONE_SITES"))
        Z_DumpSites(atoi(getenv("ZONE_SITES")));
    {
        void *bt[24];
        int n = backtrace(bt, 24);
        backtrace_symbols_fd(bt, n, 2);
    }
    end_run(1);
    exit(1);
}

void DG_Quit(void)
{
    end_run(0);
    exit(0);
}

static void end_run(int code)
{
    if (wav) {
        wav_header(wav, wav_frames);
        fclose(wav);
        wav = NULL;
    }
    if (stats) {
        static const char *const TAG[] = {"", "static", "sound", "music", "free", "level", "levspec",
                                          "purgelevel", "cache"};
        int t[9], i;
        Z_TagUsage(t, 9);
        fprintf(stderr, "host: %ld frames; zone %d KB, peak held (not purgeable) %d KB; now:", frames,
                zone_kb, Z_PeakUsed() / 1024);
        for (i = 1; i < 9; i++)
            if (t[i])
                fprintf(stderr, " %s %d", TAG[i], t[i]);
        fprintf(stderr, "\n");
        {
            extern int r_peak[4];
            extern int w_stat_max_frame_bytes, w_stat_misses, w_stat_frames, w_stat_max_frame_misses;
            fprintf(stderr, "host: peaks: visplanes %d, openings %d, drawsegs %d, vissprites %d\n",
                    r_peak[0], r_peak[1], r_peak[2], r_peak[3]);
            fprintf(stderr, "host: packed lumps: most in a frame %d KB, unpacks %d in %d frames (most %d in "
                    "one)\n", w_stat_max_frame_bytes / 1024, w_stat_misses, w_stat_frames, w_stat_max_frame_misses);
        }
        if (getenv("UNPACKS")) {
            extern void W_DumpUnpacks(int);
            W_DumpUnpacks(atoi(getenv("UNPACKS")));
        }
        if (getenv("ZONE_SITES"))
            Z_DumpSites(atoi(getenv("ZONE_SITES")));
    }
    exit(code);
}

int main(int argc, char **argv)
{
    const char *dpk = "build/doom.dpk";
    char **rest = calloc((size_t)argc + 1, sizeof *rest);
    int i, nrest = 0;
    FILE *f;
    rest[nrest++] = argv[0];
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-dpk") && i + 1 < argc)
            dpk = argv[++i];
        else if (!strcmp(argv[i], "-zone") && i + 1 < argc)
            zone_kb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-frames") && i + 1 < argc)
            max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "-shotevery") && i + 1 < argc)
            shot_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "-shot") && i + 1 < argc) {
            char *p = argv[++i];
            while (*p && nshots < 64) {
                shots[nshots++] = strtol(p, &p, 10);
                if (*p == ',')
                    p++;
            }
        } else if (!strcmp(argv[i], "-out") && i + 1 < argc)
            out_dir = argv[++i];
        else if (!strcmp(argv[i], "-script") && i + 1 < argc)
            load_script(argv[++i]);
        else if (!strcmp(argv[i], "-wav") && i + 1 < argc) {
            wav = fopen(argv[++i], "w+b");
            if (wav)
                wav_header(wav, 0);
        } else if (!strcmp(argv[i], "-stats"))
            stats = 1;
        else
            rest[nrest++] = argv[i];
    }
    f = fopen(dpk, "rb");
    if (!f) {
        fprintf(stderr, "host: %s: cannot open (tools/wadpack.py makes it)\n", dpk);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    pack_len = ftell(f);
    rewind(f);
    pack = malloc((size_t)pack_len);
    if (fread(pack, 1, (size_t)pack_len, f) != (size_t)pack_len)
        return 2;
    fclose(f);
    {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", out_dir);
        if (system(cmd))
            return 2;
    }
    doomgeneric_Create(nrest, rest);
    for (;;)
        doomgeneric_Tick();
}
