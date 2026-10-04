// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CONNECTION_FAILURE_LIMIT 3
#define CONNECTION_RETRY_MIN_S 900U

typedef struct {
    uint8_t failures;
} connection_retry_t;

static inline void connection_retry_begin(connection_retry_t *state, bool deep_sleep) {
    if (!deep_sleep) state->failures = 0;
}

// Count wakes, not individual requests within a wake.
static inline uint32_t connection_retry_sleep(connection_retry_t *state, bool connected,
                                             uint32_t next_s, uint32_t saved_s) {
    if (connected) {
        state->failures = 0;
        return next_s;
    }
    if (state->failures < CONNECTION_FAILURE_LIMIT) state->failures++;
    if (state->failures < CONNECTION_FAILURE_LIMIT) return next_s;
    if (next_s < saved_s) next_s = saved_s;
    if (next_s < CONNECTION_RETRY_MIN_S) next_s = CONNECTION_RETRY_MIN_S;
    return next_s;
}
