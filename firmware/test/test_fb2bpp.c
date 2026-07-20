// test_fb2bpp.c — host unit test for the pure framebuffer packer
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <stdio.h>
#include "fb2bpp.h"

#define N 30000

int main(void) {
    static uint8_t fb[N];

    fb_fill(fb, 0); assert(fb[0] == 0x00 && fb[N - 1] == 0x00);
    fb_fill(fb, 1); assert(fb[0] == 0x55 && fb[N - 1] == 0x55);
    fb_fill(fb, 2); assert(fb[0] == 0xAA && fb[N - 1] == 0xAA);
    fb_fill(fb, 3); assert(fb[0] == 0xFF && fb[N - 1] == 0xFF);

    fb_bars(fb);
    // first byte is leftmost bar (colour 0), last byte is rightmost bar (colour 3)
    assert(fb[0] == 0x00);
    assert(fb[N - 1] == 0xFF);

    printf("PASS\n");
    return 0;
}
