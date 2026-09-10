// epd_init_seq.h — PicPak UC81xx-class panel init sequence
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
//
// Row format: { cmd, n_data, data[0..n-1] }. Iterate by array size — 0xFF is a
// VALID command byte for this controller, so there is NO sentinel terminator.
//
// Power-ON (0x04) and Display-Refresh (0x12) are issued by epd_display(),
// not from this table.
#pragma once
#include <stdint.h>

// Full panel init sequence for the native (non-fast) refresh path. Used for all panels.
static const uint8_t EPD_INIT_NATIVE[] = {
    0x00, 2, 0x07, 0x29,              // PSR  — panel setting
    0x01, 2, 0x07, 0x00,             // PWR  — power setting
    0x03, 3, 0x10, 0x54, 0x44,       // PFS  — power-off/frame sequence
    0x06, 3, 0xC0, 0xC0, 0xC0,       // BTST — booster soft-start
    0x30, 1, 0x08,                   // PLL  — clock (dynamic frame rate)
    0x41, 1, 0x00,                   // TSE  — temperature sensor enable/config
    0x50, 1, 0x37,                   // CDI  — VCOM & data interval
    0x61, 4, 0x01, 0x90, 0x01, 0x2C, // TRES — resolution 400x300
    0x65, 4, 0x00, 0x00, 0x00, 0x00, // GSST — window start
    0xE3, 1, 0x22,                   // PWS  — power saving
    0xE7, 1, 0x1C,                   // vendor
    0xE9, 1, 0x01,                   // vendor
    0xFF, 1, 0xA5,                   // vendor
    0xEF, 8, 0x01, 0x32, 0x08, 0x32, 0x0A, 0x32, 0x0F, 0x19, // vendor
    0xFD, 1, 0x01,                   // vendor
    0xE8, 1, 0x00,                   // vendor
    0xDF, 1, 0x3C,                   // vendor
    0xDC, 1, 0x00,                   // vendor
    0xDD, 1, 0x01,                   // vendor
    0xDE, 1, 0x14,                   // vendor
    0xFF, 1, 0xE3,                   // vendor (second 0xFF write)
};
