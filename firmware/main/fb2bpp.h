// fb2bpp.h — pure 2bpp framebuffer helpers (host-testable, no ESP deps)
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdint.h>

// Fill the whole 30,000-byte framebuffer with one palette colour (0..3).
void fb_fill(uint8_t *fb, uint8_t color);

// Four vertical colour bars (Black/White/Yellow/Red across the width).
void fb_bars(uint8_t *fb);
