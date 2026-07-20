// mqtt_parse.h — pure MQTT payload/URI helpers (host-testable, no ESP deps).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Extract the frame URL from a retained frame/bin payload. Accepts either a
// bare http(s):// URL (surrounding whitespace trimmed) or a JSON object with
// a string "url" field (the Tesserae esp32_bin renderer publishes the JSON
// form). `data` need not be NUL-terminated; only `len` bytes are read.
bool mqtt_extract_url(const char *data, size_t len, char *dst, size_t dst_sz);

// Pull a top-level integer field "<key>": <int> out of a JSON payload.
// Tolerates whitespace and an optional leading minus.
bool mqtt_extract_int(const char *data, size_t len, const char *key, int32_t *out);

// esp-mqtt's URI parser rejects bare host:port; users typing the broker into
// the portal routinely leave the scheme off. Prepend "mqtt://" in place when
// none of mqtt:// mqtts:// ws:// wss:// is present. Empty input is left alone.
void mqtt_normalize_uri(char *uri, size_t uri_sz);
