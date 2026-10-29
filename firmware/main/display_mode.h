// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdbool.h>

typedef enum {
    DISPLAY_NATIVE = 2,
    DISPLAY_FAST_5S = 0,
    DISPLAY_FAST_10S = 1,
} display_mode_t;

static inline bool display_mode_valid(display_mode_t mode) {
    return mode == DISPLAY_NATIVE || mode == DISPLAY_FAST_5S ||
           mode == DISPLAY_FAST_10S;
}
