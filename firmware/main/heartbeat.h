// heartbeat.h — build the Tesserae status JSON.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_system.h"   // esp_reset_reason_t
#include <stddef.h>
#include <stdint.h>

// Build the /status heartbeat JSON. `button` (NULL/"" = none) plus
// `button_event_id` add a physical-button report as a fallback delivery path when
// the /frame button dispatch wasn't acknowledged (server dedups by event id).
void heartbeat_json(char *dst, size_t dst_sz, int sleep_interval_s,
                    esp_reset_reason_t reset_reason,
                    const char *button, uint32_t button_event_id);
