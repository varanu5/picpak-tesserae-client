// framebuf.h — the single frame staging buffer, shared by the transports.
// Only one transport runs per boot (NVS mode switch in main.c), so sharing
// one static EPD_FB_BYTES buffer instead of one per handler saves 30 KB of
// .bss on a PSRAM-less C3.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdint.h>

uint8_t *framebuf(void);   // EPD_FB_BYTES bytes of static SRAM
