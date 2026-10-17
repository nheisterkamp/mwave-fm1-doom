// SPDX-License-Identifier: GPL-3.0-only
/* 320x200 -> 240x180: each 4 source columns give 3 (weights 3:1, 1:1, 1:3), each 10 source rows give 9
 * (linear, the source position of row r is 10r/9). Blending works on "spread" RGB565, the green field moved
 * to the top half (0x07E0F81F): a weighted sum of two up to x32 never carries between the fields. */
#include "lcd_conv.h"

#define SRC_W 320
#define SRC_H 200
#define SPREAD(c) ((((uint32_t)(c) << 16) | (c)) & 0x07E0F81Fu)

static uint32_t pal_spread[256];
static uint32_t row_a[VIEW_W], row_b[VIEW_W];   /* filtered source rows: row_a = src_a, row_b = src_a + 1 */
static int32_t src_a = -2;

void lcd_conv_palette(const uint8_t *rgb)
{
    uint32_t i;
    for (i = 0; i < 256u; i++) {
        uint32_t c = ((uint32_t)(rgb[3 * i] >> 3) << 11) | ((uint32_t)(rgb[3 * i + 1] >> 2) << 5) |
                     (uint32_t)(rgb[3 * i + 2] >> 3);
        pal_spread[i] = SPREAD(c);
    }
    src_a = -2;                                   /* the kept rows were in the old colours */
}

static void filter_row(const uint8_t *s, uint32_t *d)   /* 320 indices -> 240 spread pixels */
{
    uint32_t x;
    for (x = 0; x < SRC_W; x += 4, s += 4, d += 3) {
        uint32_t a = pal_spread[s[0]], b = pal_spread[s[1]], c = pal_spread[s[2]], e = pal_spread[s[3]];
        d[0] = ((a * 3u + b) >> 2) & 0x07E0F81Fu;
        d[1] = ((b + c) >> 1) & 0x07E0F81Fu;
        d[2] = ((c + e * 3u) >> 2) & 0x07E0F81Fu;
    }
}

static void load_rows(const uint8_t *frame, int32_t a)  /* row_a = source row a, row_b = a + 1 */
{
    if (a == src_a)
        return;
    if (a == src_a + 1) {
        uint32_t x;
        for (x = 0; x < VIEW_W; x++)
            row_a[x] = row_b[x];
    } else
        filter_row(frame + (uint32_t)a * SRC_W, row_a);
    filter_row(frame + (uint32_t)(a + 1 < SRC_H ? a + 1 : a) * SRC_W, row_b);
    src_a = a;
}

void lcd_conv_rows(const uint8_t *frame, uint32_t y0, uint32_t n, uint16_t *out)
{
    uint32_t r, x;
    if (y0 == 0)
        src_a = -2;                               /* a new frame */
    for (r = y0; r < y0 + n; r++, out += VIEW_W) {
        uint32_t pos = r * 10u * 32u / 9u;        /* source row in 1/32 */
        uint32_t w = pos & 31u, iw = 32u - w;
        load_rows(frame, (int32_t)(pos >> 5));
        for (x = 0; x < VIEW_W; x++) {
            uint32_t v = ((row_a[x] * iw + row_b[x] * w) >> 5) & 0x07E0F81Fu;
            uint32_t c = (v | (v >> 16)) & 0xFFFFu;
            out[x] = (uint16_t)((c >> 8) | (c << 8));
        }
    }
}
