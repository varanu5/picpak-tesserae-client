// framebuf.c — see framebuf.h.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "framebuf.h"
#include "board.h"

static uint8_t s_frame[EPD_FB_BYTES];

uint8_t *framebuf(void) { return s_frame; }
