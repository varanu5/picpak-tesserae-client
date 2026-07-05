// fb2bpp.c — pure 2bpp framebuffer helpers (host-testable, no ESP deps)
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fb2bpp.h"
#include "board.h"

// 2 bits/pixel, 4 pixels packed per byte. A solid colour c fills every
// 2-bit field: byte = c*0x55  (0x00,0x55,0xAA,0xFF). Order-independent.
void fb_fill(uint8_t *fb, uint8_t color) {
    uint8_t b = (uint8_t)((color & 3) * 0x55);
    for (int i = 0; i < EPD_FB_BYTES; i++) fb[i] = b;
}

// Four equal vertical bars, one colour each, left→right: 0,1,2,3.
void fb_bars(uint8_t *fb) {
    const int bytes_per_row = EPD_W / 4;   // 100 bytes/row, 4 px/byte
    for (int y = 0; y < EPD_H; y++)
        for (int bx = 0; bx < bytes_per_row; bx++) {
            int col = bx / (bytes_per_row / 4);   // 0..3 across the row
            if (col > 3) col = 3;
            fb[y * bytes_per_row + bx] = (uint8_t)((col & 3) * 0x55);
        }
}
