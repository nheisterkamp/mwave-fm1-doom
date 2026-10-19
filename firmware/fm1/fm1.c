/* SPDX-License-Identifier: GPL-3.0-only */
/* Doom on the M-VAVE FM-1: the hardware side, one compilation unit (the HAL is header-only, from Felucca).
 *
 *   boot        fm1_cstart: watchdog, boot-loop guard, fault vectors, RAM sections; fm1_main: flash, screen, keys,
 *               sound, USB, then the game or the data mode. Two boots in a row that die in their first 30 s: the
 *               data mode (no game; the installer and the upload still work); four: UBOOT, the chip's own
 *   screen      ST7789 240x240 on SPI1: each frame is turned into 240x180 RGB565 (lcd_conv.c) a strip at a time,
 *               the next strip converted while DMA sends the last (LCD_ASYNC=1: the whole frame in one buffer,
 *               sent while the next frame renders; 86 KiB more RAM)
 *   sound       ALNK0 I2S, 44.1 kHz: the interrupt pulls the engine's mix (i_sound.c), MASTER sets the level
 *   keys        TIMER5 scans the matrix at 10 kHz; fm1_input_map.c turns keys, buttons and knobs into events
 *   USB         MIDI (+ serial console): the M-UPGRADE update entry (ota.c), the UBOOT soft key, and the game
 *               data upload (dpk_service, tools/fm1_doom.py)
 *   game data   part a of the DPK image is linked in (dpk_a.S); parts b and c sit in flash above the app,
 *               written by the upload (data mode). Without them the FM-1 waits in data mode.
 *
 * Flash map (offsets):  0x4000.. app (this firmware + part a)   0x93000..0xE0000 part b   0xE0000..0xE5000 update
 * staging (ota.c)   0xE5000..0xFC000 part c   0xFF000 key_mac (never written). */

/* ------------------------------------------------------------ build options --- */
#define FELUCCA_CDC 1                /* USB serial: the log (DG_Log) */
#define FELUCCA_CDC_DEFAULT 1
#define FELUCCA_OTA 1                /* the update entry: the installer can replace this firmware */
#define FELUCCA_OTA_DRYRUN 0
#define FELUCCA_UAC 0
#define FELUCCA_UART 0
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_666"        /* package identity (tools/build.py) */
#endif
#ifndef DOOM_FM1_VERSION
#define DOOM_FM1_VERSION "dev"
#endif
#ifndef LCD_ASYNC
#define LCD_ASYNC 0
#endif
#ifndef LCD_BAUD
#define LCD_BAUD 4u                  /* SPI1 = 60 MHz / (BAUD + 1): 4 = 12 MHz, as Felucca */
#endif
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

/* ------------------------------------------------------------------- HAL --- */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"
/* the flash driver may erase / write the data areas (parts b, c) and the update staging, nothing else */
#define DPK_B_LO 0x00093000u
#define DPK_B_HI 0x000E0000u
#define DPK_C_LO 0x000E5000u
#define DPK_C_HI 0x000FC000u
#define FL_RANGE_OK(off, n) (FL_IN(off, n, DPK_B_LO, DPK_B_HI) || FL_IN(off, n, DPK_C_LO, DPK_C_HI) || \
                             FL_IN(off, n, FL_OTA_LO, FL_OTA_HI))
#include "fm1_flash.h"
#include "font5x7.h"                 /* generated (tools/gen_font.py) */

#include "libc.c"                    /* Felucca's memset / memcpy / memcmp */
#include "usb.c"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "w_wad.h"
#include "fm1_input_map.h"
#include "lcd_conv.h"

extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[], _lowbss_start[], _lowbss_end[];
extern uint8_t _zone_start[], _zone_end[];
extern const uint8_t dpk_part_a[];   /* dpk_a.S: part a of the image, linked in */

/* survives resets: the boot-loop guard, and a request for the data mode from the running game. Not static
 * (as fm1_irq.h fm1_crash): .noinit holds whatever the last run or power-on left, and a file-local object lets
 * the compiler assume its zero start and only the values written, shrinking the magic to a flag byte and
 * keeping random power-on RAM as a valid record (a first build went to UBOOT at every power-on) */
#define BOOTGUARD_MAGIC 0x44424754u  /* "DBGT" */
struct { uint32_t magic, pending, failed, data_mode; } bootguard __attribute__((section(".noinit")));

static uint8_t flash_ok;             /* the expected 1 MiB part answered at boot */
static volatile uint32_t audio_gain = 160;   /* 0..256, from MASTER */

/* ----------------------------------------------------------- the screen --- */

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define SWAP16(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
#if LCD_ASYNC
#define LCD_CHUNK_ROWS 36u           /* one DMA: 240 x 36 x 2 = 17280 B; 5 make the 180 rows */
_Static_assert(VIEW_H % LCD_CHUNK_ROWS == 0, "whole chunks");
static uint16_t lcd_fb[VIEW_W * VIEW_H] __attribute__((section(".pool"), aligned(4)));   /* the frame in flight */
#else
#define LCD_STRIP_ROWS 12u           /* 15 strips of 240 x 12 x 2 = 5760 B, two buffers */
_Static_assert(VIEW_H % LCD_STRIP_ROWS == 0, "whole strips");
static uint16_t lcd_strip[2][VIEW_W * LCD_STRIP_ROWS] __attribute__((section(".noinit"), aligned(4)));
#endif
static uint8_t lcd_small[16];
static uint16_t lcd_line[LCD_W];
/* the frame's DMA, run by the TIMER5 interrupt: chunk index, 0 = idle */
static volatile uint32_t lcd_chunk, lcd_chunks_left;

static void lcd_spin_ms(uint32_t ms) { fm1_delay_ms(ms); }

static void lcd_wait_frame(void)     /* the frame in flight is out (the interrupt sends it) */
{
    while (*(volatile uint32_t *)&lcd_chunks_left)
        ;
}

static void lcd_cmd(uint8_t c, const uint8_t *d, uint32_t n)   /* polled; never while a frame is in flight */
{
    uint32_t i;
    fm1_lcd_send_cmd(c);
    fm1_lcd_wait();
    fm1_lcd_deselect();
    if (!n)
        return;
    for (i = 0; i < n; i++)
        lcd_small[i] = d[i];
    fm1_lcd_send_data(lcd_small, n);
    fm1_lcd_wait();
    fm1_lcd_deselect();
}

static void lcd_window(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint8_t a[4];
    a[0] = (uint8_t)(x0 >> 8); a[1] = (uint8_t)x0; a[2] = (uint8_t)(x1 >> 8); a[3] = (uint8_t)x1;
    lcd_cmd(0x2A, a, 4);
    a[0] = (uint8_t)(y0 >> 8); a[1] = (uint8_t)y0; a[2] = (uint8_t)(y1 >> 8); a[3] = (uint8_t)y1;
    lcd_cmd(0x2B, a, 4);
    lcd_cmd(0x2C, 0, 0);
}

static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t rgb)
{
    uint32_t i;
    lcd_wait_frame();
    for (i = 0; i < w; i++)
        lcd_line[i] = SWAP16(rgb);
    lcd_window(x, y, x + w - 1u, y + h - 1u);
    for (i = 0; i < h; i++) {
        fm1_lcd_send_data(lcd_line, w * 2u);
        fm1_lcd_wait();
        fm1_lcd_deselect();
    }
}

/* text in the 5x7 font, scale s: a cell is 6s x 8s */
static void lcd_text(uint32_t x, uint32_t y, uint32_t s, const char *str, uint16_t fg, uint16_t bg)
{
    lcd_wait_frame();
    for (; *str && x + 6u * s <= LCD_W; str++, x += 6u * s) {
        uint32_t c = (uint8_t)*str, r, col, k;
        if (c >= 'a' && c <= 'z')
            c -= 32u;
        if (c < 32u || c > 95u)
            c = '?';
        lcd_window(x, y, x + 6u * s - 1u, y + 8u * s - 1u);
        for (r = 0; r < 8u; r++) {
            uint32_t bits = r < 7u ? FONT5X7[c - 32u][r] : 0u;
            for (col = 0; col < 6u; col++)
                for (k = 0; k < s; k++)
                    lcd_line[col * s + k] = SWAP16((col < 5u && (bits >> (4u - col)) & 1u) ? fg : bg);
            for (k = 0; k < s; k++) {
                fm1_lcd_send_data(lcd_line, 12u * s);
                fm1_lcd_wait();
                fm1_lcd_deselect();
            }
        }
    }
}

static void lcd_text_center(uint32_t y, uint32_t s, const char *str, uint16_t fg, uint16_t bg)
{
    uint32_t n = 0;
    while (str[n])
        n++;
    lcd_text(n * 6u * s < LCD_W ? (LCD_W - n * 6u * s) / 2u : 0u, y, s, str, fg, bg);
}

static void lcd_init(void)
{
    static const uint8_t colmod = 0x55, madctl = 0x00;
    fm1_lcd_hw_init();
    lcd_spin_ms(50);
    lcd_cmd(0x01, 0, 0);             /* SWRESET */
    lcd_spin_ms(150);
    lcd_cmd(0x11, 0, 0);             /* SLPOUT */
    lcd_spin_ms(120);
    lcd_cmd(0x3A, &colmod, 1);       /* RGB565 */
    lcd_cmd(0x36, &madctl, 1);       /* top-left origin, RGB */
    lcd_cmd(0x21, 0, 0);             /* INVON: an IPS panel */
    lcd_cmd(0x13, 0, 0);             /* NORON */
    fm1_lcd_baud(LCD_BAUD);
    lcd_fill(0, 0, LCD_W, LCD_H, 0);
    lcd_cmd(0x29, 0, 0);             /* DISPON */
}

#if LCD_ASYNC
/* TIMER5: the next chunk of the frame when the last is out */
static void lcd_frame_tick(void)
{
    uint32_t n;
    if (!lcd_chunks_left || !(FM1_LCD_SPI_CON & 0x8000u))
        return;
    FM1_LCD_SPI_CON |= 0x4000u;
    fm1_lcd_deselect();
    if (--lcd_chunks_left == 0)
        return;
    n = lcd_chunk++;
    fm1_lcd_send_data(lcd_fb + n * LCD_CHUNK_ROWS * VIEW_W, LCD_CHUNK_ROWS * VIEW_W * 2u);
}

static void lcd_frame(const uint8_t *frame)
{
    lcd_wait_frame();
    lcd_conv_rows(frame, 0, VIEW_H, lcd_fb);
    lcd_window(0, VIEW_Y, VIEW_W - 1u, VIEW_Y + VIEW_H - 1u);
    lcd_chunk = 1;
    fm1_irq_off();
    lcd_chunks_left = VIEW_H / LCD_CHUNK_ROWS;        /* the tick counts each one done */
    fm1_lcd_send_data(lcd_fb, LCD_CHUNK_ROWS * VIEW_W * 2u);
    fm1_irq_on();
}
#else
static void lcd_frame_tick(void) {}

static void lcd_frame(const uint8_t *frame)  /* a strip converts while the last one goes out */
{
    uint32_t y, k = 0;
    lcd_window(0, VIEW_Y, VIEW_W - 1u, VIEW_Y + VIEW_H - 1u);
    for (y = 0; y < VIEW_H; y += LCD_STRIP_ROWS, k ^= 1u) {
        lcd_conv_rows(frame, y, LCD_STRIP_ROWS, lcd_strip[k]);
        if (y) {
            fm1_lcd_wait();
            fm1_lcd_deselect();
        }
        fm1_lcd_send_data(lcd_strip[k], VIEW_W * LCD_STRIP_ROWS * 2u);
    }
    fm1_lcd_wait();
    fm1_lcd_deselect();
}
#endif

/* ----------------------------------------------------------- the sound --- */

#define HALF_FRAMES 128u             /* I2S half buffer: 2.9 ms */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define AUDIO_RATE 44118             /* the codec's clock: 24 MHz / 544 */
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static int16_t amix[2u * HALF_FRAMES];
static uint8_t audio_on;

void fm1_alnk0_irq(void)             /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        int32_t *o = &abuf[fm1_audio_free_half() * HALF_WORDS];
        uint32_t i, g = audio_gain;
        if (audio_on)
            I_MixSound(amix, HALF_FRAMES, AUDIO_RATE);
        else
            memset(amix, 0, sizeof amix);
        for (i = 0; i < HALF_WORDS; i++)
            o[i] = ((int32_t)amix[i] * (int32_t)g) >> 1;   /* Q15 x gain/256 << 7: 24 bit, -6 dBFS at most */
        fm1_audio_ack_half();
    }
}
extern void isr_alnk0(void);

static void audio_silence(void)      /* before the flash stops the CPU: the DMA would loop stale audio */
{
    memset(abuf, 0, sizeof abuf);
}

static void master_poll(void)        /* the MASTER pot -> level, a square law */
{
    int32_t v = fm1_adc_read(FM1_ADC_MASTER);
    if (v >= 0)
        audio_gain = (uint32_t)(v * v) / 4096u;      /* 0..255 */
}

/* --------------------------------------------------------- time, keys --- */

static void lcd_frame_tick(void);
void fm1_timer5_irq(void)            /* 10 kHz: key scan, ms, LCD DMA chunks, USB at 2 kHz */
{
    static uint32_t sub, last, acc;
    uint32_t t = fm1_ticks();
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t - last;
    last = t;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        fm1_ms++;
    }
    lcd_frame_tick();
    if (++sub == 5u) {
        sub = 0;
        usb_poll();
    }
}
extern void isr_timer5(void);

static uint32_t keys_seen, buttons_seen;

/* the LEDs: a glow on every key and button that does something in Doom, lit while held. Controls and LED ids
 * count alike: buttons 0..13, then the note keys */
static void led_glow(uint32_t id, int on)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on)
                    fm1_led_dim[p] |= (uint8_t)(1u << r);
                else
                    fm1_led_dim[p] &= (uint8_t)~(1u << r);
            }
}

static void leds_guide(void)
{
    uint32_t id;
    fm1_led_dim_level(0);
    for (id = 0; id < FM1C_COUNT; id++)
        led_glow(id, fm1_control_used((int)id));
}

static void input_poll(void)         /* key and button changes, knob clicks -> fm1_input_map.c */
{
    uint32_t notes = fm1_in.notes, buttons = fm1_in.buttons, d, i;
    for (d = notes ^ keys_seen, i = 0; d; i++, d >>= 1)
        if (d & 1u) {
            fm1_map_control(FM1C_KEY0 + (int)i, (notes >> i) & 1u);
            fm1_led_key(FM1C_KEY0 + i, (notes >> i) & 1u);
        }
    for (d = buttons ^ buttons_seen, i = 0; d; i++, d >>= 1)
        if (d & 1u) {
            fm1_map_control((int)i, (buttons >> i) & 1u);
            fm1_led_key(i, (buttons >> i) & 1u);
        }
    keys_seen = notes;
    buttons_seen = buttons;
    for (i = 0; i < FM1_NENC; i++)
        fm1_map_encoder((int)i, fm1_enc_take(i));
}

/* ------------------------------------------------------- flash, update --- */

static int fl_read(uint32_t off, void *dst, uint32_t n)   /* 256-byte IRQ-off windows: the sound keeps up */
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}

static int fl_erase(uint32_t off)
{
    uint32_t took, f;
    int rc;
    if (!FL_RANGE_OK(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    f = irq_save();
    audio_silence();
    fm1__led_lines(0);               /* (the scan stops while the IRQs are off: no column left lit) */
    rc = FL_FAR(fl_erase4k_ram)(off, &took);
    irq_restore(f);
    return rc;
}

static void fault_screen(const char *title, const char *l1, const char *l2);

/* ota.c hooks */
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_erase(uint32_t off)
{
    if (!FL_IN(off, 0x1000u, FL_OTA_LO, FL_OTA_HI))
        return -8;
    return fl_erase(off);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!FL_IN(off, n, FL_OTA_LO, FL_OTA_HI))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return fl_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    lcd_fill(0, 0, LCD_W, LCD_H, 0);
    lcd_text_center(92, 3, "UPDATE", C565(255, 255, 255), 0);
    if (step < 9u)
        lcd_text_center(130, 2, STEP[step < 6u ? step : 0], C565(255, 160, 0), 0);
    else {
        lcd_text_center(130, 2, code == 1 ? "DRY RUN OK" : "FAILED", C565(255, 60, 60), 0);
        fm1_delay_ms(1500);
    }
}
static void ota_commit(const uint8_t *parm)
{
    bootguard.pending = 0;           /* an intended reset */
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}
#include "ota.c"

static void enter_uboot(const char *why)
{
    audio_on = 0;
    fault_screen("UBOOT", why, "");
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;
    fm1_enter_uboot();
}

static void reboot(uint32_t data_mode)
{
    bootguard.pending = 0;
    bootguard.data_mode = data_mode;
    fm1_delay_ms(50);                /* (a last answer leaves: the USB poll runs meanwhile) */
    usb_detach();
    fm1_delay_ms(30);
    fm1_reboot();
}

/* ------------------------------------------------- game data upload --- */

static uint8_t in_data_mode;
static int dpk_check_result = -100;
static int dk_erase(uint32_t off) { return fl_erase(off); }
static int dk_write(uint32_t off, const uint8_t *p, uint32_t n) { return fl_write(off, p, n); }
static const uint8_t *dk_flash(uint32_t off) { return fm1_xip_ptr(off); }
static void dk_idle(void) { fm1_wdt_feed(); }
static int dpk_check(void);
#include "dpk_proto.c"

/* ---------------------------------------------------------- the service --- */

static uint32_t uboot_t0, last_master;

/* the FM-1's own business, between frames and while the game waits: watchdog, USB, update, UBOOT keys */
static void service(void)
{
    uint32_t both = (1u << FM1_BTN_OCT_DOWN) | (1u << FM1_BTN_OCT_UP);
    fm1_wdt_feed();
    usb_retry(fm1_ms);
    if (fm1_ms > 30000u && bootguard.pending) {   /* 30 s up: this boot is good */
        bootguard.pending = 0;
        bootguard.failed = 0;
    }
    if (fm1_ms - last_master >= 50u) {
        last_master = fm1_ms;
        master_poll();
    }
    dpk_service();
    ota_service();
    if (usb.ota_req) {
        usb.ota_req = 0;
        audio_on = 0;
        if (flash_ok)
            ota_session();           /* returns only if nothing was committed */
        lcd_fill(0, 0, LCD_W, LCD_H, 0);
        audio_on = 1;
    }
    if (usb.uboot_req)
        enter_uboot("USB");
    if ((fm1_in.buttons & both) != both)       /* OCT- and OCT+ held 5 s: UBOOT (recovery) */
        uboot_t0 = fm1_ms;
    else if (fm1_ms - uboot_t0 > 5000u)
        enter_uboot("OCT- OCT+");
}

/* --------------------------------------------------------------- faults --- */

static void fault_screen(const char *title, const char *l1, const char *l2)
{
    lcd_chunks_left = 0;             /* (a frame in flight is dropped) */
    fm1_lcd_wait();
    fm1_lcd_deselect();
    lcd_fill(0, 0, LCD_W, LCD_H, C565(120, 0, 0));
    lcd_text_center(60, 3, title, C565(255, 255, 255), C565(120, 0, 0));
    lcd_text(8, 110, 1, l1, C565(255, 255, 255), C565(120, 0, 0));
    lcd_text(8, 124, 1, l2, C565(255, 255, 255), C565(120, 0, 0));
}

static void hex8(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char a[24], b[24];
    fm1_audio_stop();
    memcpy(a, "PC ", 3);
    hex8(a + 3, c->pc);
    memcpy(b, "RETS ", 5);
    hex8(b + 5, c->rets);
    fault_screen("CRASH", a, b);
    fm1_delay_ms(4000);
    fm1_reboot();
}

/* ----------------------------------------------------- the engine's platform --- */

void DG_Init(void) {}

void DG_DrawFrame(const uint8_t *screen)
{
    lcd_frame(screen);
    service();
}

void DG_SetPalette(const uint8_t *rgb)
{
    lcd_wait_frame();                /* (the frame in flight was converted with the old one: keep it) */
    lcd_conv_palette(rgb);
}

uint32_t DG_GetTicksMs(void)
{
    return fm1_ms;
}

void DG_SleepMs(uint32_t ms)
{
    uint32_t t0 = fm1_ms;
    while (fm1_ms - t0 < ms)
        service();
}

int DG_GetEvent(event_t *ev)
{
    input_poll();
    return fm1_map_event(ev);
}

uint8_t *DG_ZoneBase(int *size)
{
    *size = (int)(_zone_end - _zone_start);
    return _zone_start;
}

const void *DG_PackPart(int part)
{
    if (part == 0)
        return dpk_part_a;
    if (!flash_ok)
        return 0;
    return fm1_xip_ptr(part == 1 ? DPK_B_LO : DPK_C_LO);
}

void DG_Log(const char *s)           /* the USB serial console */
{
    if (!cdc.dtr)
        return;
    for (; *s; s++) {
        if (co_w - co_r >= CO_N)
            return;
        cdc_out[co_w % CO_N] = (uint8_t)*s;
        RING_PUBLISH();
        co_w++;
    }
    if (co_w - co_r < CO_N - 1u) {
        cdc_out[co_w % CO_N] = '\r';
        co_w++;
        cdc_out[co_w % CO_N] = '\n';
        RING_PUBLISH();
        co_w++;
    }
}

void DG_Error(const char *msg)
{
    char l1[40], l2[40];
    uint32_t i, k;
    audio_on = 0;
    for (i = 0, k = 0; msg[i] && msg[i] != '\n' && k < 38u; i++)
        l1[k++] = msg[i];
    l1[k] = 0;
    while (msg[i] && msg[i] != '\n')
        i++;
    if (msg[i] == '\n')
        i++;
    for (k = 0; msg[i] && msg[i] != '\n' && k < 38u; i++)
        l2[k++] = msg[i];
    l2[k] = 0;
    DG_Log(msg);
    fault_screen("DOOM ERROR", l1, l2);
    lcd_text_center(200, 1, "RESTART IN 10 S", C565(255, 255, 255), C565(120, 0, 0));
    {
        uint32_t t0 = fm1_ms;
        while (fm1_ms - t0 < 10000u)
            service();
    }
    reboot(0);
    for (;;)
        ;
}

void DG_Quit(void)
{
    audio_on = 0;
    reboot(0);
    for (;;)
        ;
}

/* ------------------------------------------------------------ data mode --- */

static void data_mode(int why)
{
    char b[24];
    in_data_mode = 1;
    audio_on = 0;
    lcd_fill(0, 0, LCD_W, LCD_H, 0);
    lcd_text_center(24, 4, "DOOM", C565(220, 30, 30), 0);
    lcd_text_center(70, 2, "DATA MODE", C565(255, 255, 255), 0);
    lcd_text_center(110, 1, why ? "THE GAME DATA IS MISSING." : "READY FOR GAME DATA.", C565(255, 200, 0), 0);
    lcd_text_center(132, 1, "CONNECT USB AND RUN", C565(200, 200, 200), 0);
    lcd_text_center(146, 1, "TOOLS/FM1_DOOM.PY UPLOAD", C565(200, 200, 200), 0);
    memcpy(b, "CHECK ", 6);
    hex8(b + 6, (uint32_t)why);
    lcd_text_center(200, 1, b, C565(110, 110, 110), 0);
    lcd_text_center(224, 1, "FM-1 DOOM " DOOM_FM1_VERSION, C565(110, 110, 110), 0);
    for (;;)
        service();
}

/* the parts b and c the linked part a expects: 0 if they are there (as W_InitPack checks them) */
static int dpk_check(void)
{
    const dpk_header_t *h = (const dpk_header_t *)dpk_part_a;
    if (!flash_ok)
        return -10;
    if (h->end_a > h->end_b || h->end_b > h->total_size || h->end_b - h->end_a > DPK_B_HI - DPK_B_LO ||
        h->total_size - h->end_b > DPK_C_HI - DPK_C_LO)
        return -11;
    if (crc32_xip(fm1_xip_ptr(DPK_B_LO), h->end_b - h->end_a) != h->crc_b)
        return -4;
    if (crc32_xip(fm1_xip_ptr(DPK_C_LO), h->total_size - h->end_b) != h->crc_c)
        return -5;
    return 0;
}

/* ----------------------------------------------------------------- boot --- */

static void timer5_start(void) { fm1_timer5_start(isr_timer5, 4); }

static void fm1_main(void)
{
    uint32_t f = irq_save(), want_data;
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;   /* the 1 MiB part of every FM-1 so far */
    irq_restore(f);
    if (flash_ok) {
        fl_plain_window_init();      /* flash above 0x93000 reads as plaintext through XIP */
        ota_boot_cleanup();          /* a staging area left by an update */
    }
    lcd_init();
    fm1_lcd_backlight(1);
    lcd_text_center(100, 4, "DOOM", C565(220, 30, 30), 0);
    lcd_text_center(140, 1, "FM-1 " DOOM_FM1_VERSION, C565(160, 160, 160), 0);
    fm1_input_init();
    fm1_adc_init();
    memset(abuf, 0, sizeof abuf);
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
    usb_start();
    timer5_start();
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    master_poll();
    fm1_delay_ms(30);
    want_data = bootguard.data_mode || (fm1_in.buttons & (1u << FM1C_HOME)) ||   /* HOME held: data mode */
                bootguard.failed >= 2u;      /* the game died early twice in a row: keep out of it */
    bootguard.data_mode = 0;
    dpk_check_result = dpk_check();
    if (dpk_check_result || want_data)
        data_mode(dpk_check_result);
    audio_on = 1;
    leds_guide();
    doomgeneric_Create(0, 0);
    for (;;)
        doomgeneric_Tick();
}

void fm1_cstart(void)
{
    uint32_t *s, *d;
    fm1_time_init();
    fm1_reset_reason();
    fm1_wdt_arm(0x0D);               /* ~8 s, fed by service() */
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
        bootguard.data_mode = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 4u) {    /* even the data mode died: UBOOT (FM-1-transporter can write the flash) */
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (d = _lowbss_start; d < _lowbss_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;                     /* the flash driver, which runs from RAM */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_main();
    for (;;)
        ;
}
