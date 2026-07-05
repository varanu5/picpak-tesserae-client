// rest_handler.h — Tesserae REST transport (one wake cycle).
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_system.h"   // esp_reset_reason_t

// Run one REST wake cycle (WiFi must already be up): ensure token
// (discover/register) -> GET /frame (ETag/304/204) -> download + paint on a new
// frame -> POST /status. Returns the deep-sleep seconds the server suggested
// (next_poll_s), or the configured fallback on any non-fatal failure.
int rest_run_loop(esp_reset_reason_t reset_reason);
