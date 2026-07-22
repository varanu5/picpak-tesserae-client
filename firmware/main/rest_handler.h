// rest_handler.h — Tesserae REST transport (one wake cycle).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_system.h"   // esp_reset_reason_t

#include <stdint.h>
#include <stdbool.h>

// Run one REST wake cycle (WiFi must already be up): ensure token
// (discover/register) -> GET /frame (ETag/304/204) -> download + validate a new
// frame -> POST /status. Network I/O only — a new frame is buffered, not
// painted (see rest_pending_frame). Returns the deep-sleep seconds the server
// suggested (next_poll_s), or the configured fallback on any non-fatal failure.
//
// button_refresh: a 3 s front-button hold this wake. Adds ?button=refresh&
// button_event_id=<id> to /frame (server re-renders the current page -> fresh
// data) and drops If-None-Match so the re-render always comes back as 200.
// button_event_id lets the server dedup the press across /frame + /status.
int rest_run_loop(esp_reset_reason_t reset_reason,
                  bool button_refresh, uint32_t button_event_id);

// After rest_run_loop: the validated new frame to paint this wake, or NULL
// (304/204/error). Paint it with the radio already off, then call
// rest_frame_painted() to persist its ETag — only after a successful paint, so
// a failed paint re-fetches as a 200 next wake instead of 304-skipping forever.
const uint8_t *rest_pending_frame(void);
void rest_frame_painted(void);
