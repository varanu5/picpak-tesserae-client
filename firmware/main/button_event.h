// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdint.h>

static inline uint32_t button_event_next(uint32_t *sequence, uint32_t entropy) {
    if (!*sequence || *sequence == UINT32_MAX) *sequence = entropy ? entropy : 1;
    else ++*sequence;
    return *sequence;
}
