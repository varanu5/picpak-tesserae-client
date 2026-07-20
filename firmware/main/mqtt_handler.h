// mqtt_handler.h — Tesserae MQTT transport, Mode 0 (one wake cycle).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_system.h"   // esp_reset_reason_t

#include <stdint.h>
#include <stdbool.h>

// Run one MQTT wake cycle (WiFi must already be up), single broker session:
// connect -> subscribe frame/bin + config (retained) -> wait for the frame URL
// -> download + validate a new frame over HTTP -> publish retained heartbeat.
// Network I/O only — a new frame is buffered, not painted (see
// mqtt_pending_frame). Stops the client gracefully (no LWT) before returning,
// so main.c can wifi_stop() immediately after. Returns the deep-sleep seconds
// from NVS (the retained config topic may have updated it this session).
int mqtt_run_loop(esp_reset_reason_t reset_reason);

// After mqtt_run_loop: the validated new frame to paint this wake, or NULL
// (unchanged/no-message/error). Paint it with the radio already off, then call
// mqtt_frame_painted() to persist its URL — only after a successful paint, so
// a failed paint refetches next wake instead of skip-looping forever.
const uint8_t *mqtt_pending_frame(void);
void mqtt_frame_painted(void);
