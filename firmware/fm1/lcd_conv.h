// SPDX-License-Identifier: GPL-3.0-only
// Doom's 320x200 8-bit frame -> the panel's 240x180 RGB565 (4:3, as Doom was shown), filtered.

#pragma once
#include <stdint.h>

#define LCD_W 240
#define LCD_H 240
#define VIEW_W 240                 /* the picture on the panel */
#define VIEW_H 180
#define VIEW_Y ((LCD_H - VIEW_H) / 2)   /* black bands above and below */

/* the palette: 256 x R, G, B */
void lcd_conv_palette(const uint8_t *rgb);

/* panel rows [y0, y0 + n) of the picture (0 .. VIEW_H) from the 320x200 frame, RGB565 high byte first
 * (as the panel takes it over SPI), VIEW_W pixels a row. Rows in order from y0 = 0 are cheapest (a filtered
 * source row is kept for the next call). */
void lcd_conv_rows(const uint8_t *frame, uint32_t y0, uint32_t n, uint16_t *out);
