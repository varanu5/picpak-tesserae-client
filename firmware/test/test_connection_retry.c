// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <stdio.h>
#include "connection_retry.h"

int main(void) {
    connection_retry_t state = {0};
    // WiFi and service failures share one streak across timer and button wakes.
    assert(connection_retry_sleep(&state, false, 60, 60) == 60);
    connection_retry_begin(&state, true);
    assert(state.failures == 1);
    assert(connection_retry_sleep(&state, false, 60, 60) == 60);
    connection_retry_begin(&state, true);
    assert(connection_retry_sleep(&state, false, 60, 60) == 900);
    for (unsigned i = 0; i < 1000; i++) {
        connection_retry_begin(&state, true);
        assert(connection_retry_sleep(&state, false, 60, 60) == 900);
        assert(state.failures == 3);
    }
    // Preserve longer saved intervals even if pairing requested an earlier retry.
    assert(connection_retry_sleep(&state, false, 30, 3600) == 3600);
    assert(connection_retry_sleep(&state, false, 7200, 3600) == 7200);
    // Recovery uses the current server decision, not the extended failure sleep.
    assert(connection_retry_sleep(&state, true, 42, 3600) == 42);
    assert(state.failures == 0);
    assert(connection_retry_sleep(&state, false, 300, 300) == 300);
    assert(connection_retry_sleep(&state, true, 300, 300) == 300);
    assert(connection_retry_sleep(&state, false, 300, 300) == 300);
    assert(state.failures == 1);
    // A cold boot or restart starts a new streak.
    connection_retry_begin(&state, false);
    assert(state.failures == 0);
    assert(connection_retry_sleep(&state, false, 900, 900) == 900);
    assert(connection_retry_sleep(&state, false, 900, 900) == 900);
    assert(connection_retry_sleep(&state, false, 900, 900) == 900);
    assert(connection_retry_sleep(&state, true, 60, 60) == 60);
    assert(state.failures == 0);
    puts("connection retry tests passed");
    return 0;
}
