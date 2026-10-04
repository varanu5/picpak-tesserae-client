// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "diag.h"

#define LOG_CAPTURE_RING_BYTES 3072
#define LOG_CAPTURE_TIMEOUT_MS 10000

// Keep the serial output and capture a redacted copy in retained memory.
void log_capture_init(void);
void log_capture_paint_error(diag_paint_t error);
// Append REST capabilities and the pending failure. Returns true if a report was added.
bool log_capture_status(char *body, size_t cap, uint32_t *report_id);
void log_capture_ack_report(uint32_t report_id);
// The caller owns transport authentication, timeout and connection reuse.
typedef bool (*log_upload_fn)(const char *body, size_t len, void *context);
bool log_capture_upload(log_upload_fn send, void *context);
