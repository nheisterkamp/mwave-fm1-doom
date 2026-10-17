/* SPDX-License-Identifier: GPL-3.0-only */
/* The firmware's game data upload (firmware/fm1/dpk_proto.c) and update handshake (firmware/fm1/ota.c) on a
 * simulated 1 MiB flash, for tests/test_upload.py: SysEx messages in on stdin, out on stdout, each as a
 * 16-bit little-endian length and the bytes F0..F7.
 *
 *   tests/dpk_sim IMAGE.dpk     (its header is the firmware's linked part a) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w_wad.h"            /* dpk_header_t */

#define FELUCCA_ID "FM-1_666"
#define DPK_B_LO 0x00093000u
#define DPK_B_HI 0x000E0000u
#define DPK_C_LO 0x000E5000u
#define DPK_C_HI 0x000FC000u
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && \
                               (uint32_t)(n) <= (hi) - (uint32_t)(off))
#define FL_OTA_LO 0xE0000u
#define FL_OTA_HI 0xE5000u

static uint8_t flash[0x100000];
static uint8_t dpk_part_a[64];
static uint8_t in_data_mode;
static int dpk_check_result;
static uint32_t now_ms;

/* usb.c's side */
static uint8_t frame[640];
static uint32_t frame_len, frame_ready;
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint8_t h[2] = {(uint8_t)n, (uint8_t)(n >> 8)};
    fwrite(h, 1, 2, stdout);
    fwrite(p, 1, n, stdout);
    fflush(stdout);
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!frame_ready)
        return 0;
    *p = frame;
    *n = frame_len;
    return 1;
}
static void ota_frame_done(void) { frame_ready = 0; }

/* ota.c's hooks: only the handshake is used here */
static uint32_t ota_now_ms(void) { return now_ms += 1; }
static void ota_idle(void) {}
static int ota_erase(uint32_t off) { (void)off; return -1; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { memcpy(p, flash + off, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }
#define FELUCCA_OTA_DRYRUN 0
#define __attribute__(x)
#include "../firmware/fm1/ota.c"
#undef __attribute__

/* dpk_proto.c's hooks */
static int dk_erase(uint32_t off)
{
    if (!FL_IN(off, 0x1000u, DPK_B_LO, DPK_B_HI) && !FL_IN(off, 0x1000u, DPK_C_LO, DPK_C_HI))
        return -8;
    memset(flash + off, 0xFF, 0x1000);
    return 0;
}
static int dk_write(uint32_t off, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        flash[off + i] &= p[i];        /* NOR: programming only clears bits */
    return 0;
}
static const uint8_t *dk_flash(uint32_t off) { return flash + off; }
static void dk_idle(void) {}
static int dpk_check(void);
static void reboot(uint32_t mode)
{
    dpk_check_result = dpk_check();
    in_data_mode = mode || dpk_check_result;
}
#include "../firmware/fm1/dpk_proto.c"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int dpk_check(void)         /* as fm1.c's */
{
    uint32_t total = rd32(dpk_part_a + 16), end_a = rd32(dpk_part_a + 20), end_b = rd32(dpk_part_a + 24);
    if (crc32_xip(flash + DPK_B_LO, end_b - end_a) != rd32(dpk_part_a + 32))
        return -4;
    if (crc32_xip(flash + DPK_C_LO, total - end_b) != rd32(dpk_part_a + 36))
        return -5;
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    uint8_t h[2];
    if (!f || fread(dpk_part_a, 1, 40, f) != 40)
        return 2;
    fclose(f);
    memset(flash, 0xFF, sizeof flash);
    reboot(0);
    while (fread(h, 1, 2, stdin) == 2) {
        uint32_t n = h[0] | h[1] << 8;
        uint8_t buf[2048];
        if (n > sizeof buf || fread(buf, 1, n, stdin) != n)
            return 3;
        if (n < 2 || buf[0] != 0xF0 || buf[n - 1] != 0xF7 || n - 2 > sizeof frame)
            continue;
        memcpy(frame, buf + 1, n - 2);   /* usb.c hands over the bytes between F0 and F7 */
        frame_len = n - 2;
        frame_ready = 1;
        dpk_service();
        ota_service();
        frame_ready = 0;
    }
    return 0;
}
