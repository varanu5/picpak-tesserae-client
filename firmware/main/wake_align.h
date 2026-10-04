// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once

#include <stdbool.h>
#include <stdint.h>

void wake_align_begin(bool deep_sleep_reset, bool timer_wake);
uint32_t wake_align_epoch(double value);
void wake_align_sync(uint32_t epoch);
// Accept a REST status schedule. Zero keeps the relative sleep interval.
void wake_align_set_target(uint32_t epoch);
// Call immediately before a normal sleep, after painting and button release.
uint64_t wake_align_timer_us(uint32_t fallback_s);
