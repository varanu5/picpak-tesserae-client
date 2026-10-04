// power.h — battery ADC, deep sleep, boot button window.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "button_gesture.h"

void power_measure_battery(void);  // read + cache once (call early, before WiFi/EPD load the rail)
int  power_battery_mv(void);   // cached Li-Po mV (measures on first call if not yet cached)
int  power_battery_pct(int mv); // 0..100 via a Li-Po discharge curve (pass mv from power_battery_mv)

bool power_button_held(void);  // GPIO2 currently pressed (active-low)

// Boot button-hold gesture, classified on RELEASE (see thresholds in defaults.h).
// Run the boot-hold window: block while the button is held at boot (keeping USB
// enumerated for re-flashing), then classify the gesture by how long it was held.
// A held-to-20s returns PROVISION; released 10-20s returns MAINTENANCE; released
// 5-10s returns REFRESH; shorter returns TAP; not held at boot returns NONE.
btn_gesture_t power_boot_gesture(void);

void power_sleep_until_button(void);
void power_deep_sleep(uint32_t seconds);   // timer + button wake, then sleep (no return)
void power_scheduled_sleep(uint32_t seconds);
