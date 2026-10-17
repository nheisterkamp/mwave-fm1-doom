/* SPDX-License-Identifier: GPL-3.0-only */
/* The game data upload, the firmware's side (tools/fm1_doom.py is the host's). Included by fm1.c, and by
 * tests/dpk_sim.c on a simulated flash; the includer provides:
 *   in_data_mode, dpk_check_result, dpk_part_a[] (the image's header), DPK_B_LO/HI, DPK_C_LO/HI, FL_IN()
 *   dk_erase(off) dk_write(off, p, n) -> 0 ok; dk_flash(off) -> the flash, readable; dk_idle(); reboot(mode);
 *   dpk_check() -> 0 when parts b and c are what part a expects
 *   ota_frame_get / ota_frame_done / ota_wire_send (usb.c), ota_pack7 / ota_unpack7 / ota_rd32 / ota_wr32 (ota.c)
 *
 * SysEx F0 7D 44 4B cmd seq <pack7 payload> F7 (0x7D: non-commercial), the answer
 * F0 7D 44 4B cmd|0x40 seq status <pack7 payload> F7. tools/fm1_doom.py is the host side.
 *   01 INFO                        -> u32 version, mode (0 game, 1 data), part b lo hi, part c lo hi,
 *                                     image end_a end_b total crc_b crc_c, check (0 = parts b and c good)
 *   02 ERASE  u32 off              4 KiB sector, inside parts b / c (data mode only)
 *   03 WRITE  u32 off, <= 256 B    inside one 256-byte page (data mode only)
 *   04 CRC    u32 off, u32 len     -> u32 crc32 of the flash there
 *   05 REBOOT u8 data_mode */
#define DK_VERSION 1u
static uint8_t dk_dec[300], dk_out[64], dk_wire[2 + 80];
static uint8_t dk_page[256] __attribute__((aligned(4)));

static uint32_t crc32_xip(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu, i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        if ((i & 0xFFFFu) == 0xFFFFu)
            dk_idle();
    }
    return ~crc;
}

static void dk_reply(uint32_t cmd, uint32_t seq, uint32_t status, const uint8_t *p, uint32_t n)
{
    uint32_t w = 0;
    dk_wire[w++] = 0xF0;
    dk_wire[w++] = 0x7D;
    dk_wire[w++] = 0x44;
    dk_wire[w++] = 0x4B;
    dk_wire[w++] = (uint8_t)(cmd | 0x40u);
    dk_wire[w++] = (uint8_t)(seq & 0x7Fu);
    dk_wire[w++] = (uint8_t)(status & 0x7Fu);
    w += ota_pack7(p, n, dk_wire + w);
    dk_wire[w++] = 0xF7;
    ota_wire_send(dk_wire, w);
}

static int dk_in_parts(uint32_t off, uint32_t n)
{
    return FL_IN(off, n, DPK_B_LO, DPK_B_HI) || FL_IN(off, n, DPK_C_LO, DPK_C_HI);
}

static void dpk_service(void)        /* our frames; any other stays for ota_service */
{
    const uint8_t *p;
    uint32_t n, d, cmd, seq, status = 0, off, len;
    if (!ota_frame_get(&p, &n))
        return;
    if (n < 5u || p[0] != 0x7Du || p[1] != 0x44u || p[2] != 0x4Bu)
        return;
    cmd = p[3];
    seq = p[4];
    d = ota_unpack7(p + 5, n - 5u, dk_dec, sizeof dk_dec);
    ota_frame_done();
    if (d > sizeof dk_dec)
        d = 0;
    off = d >= 4u ? ota_rd32(dk_dec) : 0u;
    switch (cmd) {
    case 0x01: {
        const dpk_header_t *h = (const dpk_header_t *)dpk_part_a;
        ota_wr32(dk_out + 0, DK_VERSION);
        ota_wr32(dk_out + 4, in_data_mode);
        ota_wr32(dk_out + 8, DPK_B_LO);
        ota_wr32(dk_out + 12, DPK_B_HI);
        ota_wr32(dk_out + 16, DPK_C_LO);
        ota_wr32(dk_out + 20, DPK_C_HI);
        ota_wr32(dk_out + 24, h->end_a);
        ota_wr32(dk_out + 28, h->end_b);
        ota_wr32(dk_out + 32, h->total_size);
        ota_wr32(dk_out + 36, h->crc_b);
        ota_wr32(dk_out + 40, h->crc_c);
        if (in_data_mode)                       /* (in data mode the flash changes: look again) */
            dpk_check_result = dpk_check();
        ota_wr32(dk_out + 44, (uint32_t)dpk_check_result);
        dk_reply(cmd, seq, 0, dk_out, 48);
        return;
    }
    case 0x02:
        status = !in_data_mode ? 3u : d < 4u || (off & 0xFFFu) || !dk_in_parts(off, 0x1000u) ? 2u :
                 dk_erase(off) ? 1u : 0u;
        break;
    case 0x03:
        len = d >= 4u ? d - 4u : 0u;
        if (!in_data_mode)
            status = 3;
        else if (!len || len > 256u || (off & 0xFFu) + len > 256u || !dk_in_parts(off, len))
            status = 2;
        else {
            memcpy(dk_page, dk_dec + 4, len);
            status = dk_write(off, dk_page, len) ? 1u : 0u;
        }
        break;
    case 0x04:
        len = d >= 8u ? ota_rd32(dk_dec + 4) : 0u;
        if (!dk_in_parts(off, len))
            status = 2;
        else {
            ota_wr32(dk_out, crc32_xip(dk_flash(off), len));
            dk_reply(cmd, seq, 0, dk_out, 4);
            return;
        }
        break;
    case 0x05:
        dk_reply(cmd, seq, 0, 0, 0);
        reboot(d >= 1u && dk_dec[0]);
        return;
    default:
        status = 4;
        break;
    }
    dk_reply(cmd, seq, status, 0, 0);
}
