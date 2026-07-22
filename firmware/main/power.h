// power.h — battery ADC, deep sleep, boot button window.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdint.h>
#include <stdbool.h>

void power_measure_battery(void);  // read + cache once (call early, before WiFi/EPD load the rail)
int  power_battery_mv(void);   // cached Li-Po mV (measures on first call if not yet cached)
int  power_battery_pct(int mv); // 0..100 via a Li-Po discharge curve (pass mv from power_battery_mv)

bool power_button_held(void);  // GPIO2 currently pressed (active-low)

// Boot button-hold gesture, classified on RELEASE (see thresholds in defaults.h).
typedef enum {
    BTN_GESTURE_NONE = 0,   // no hold / quick tap -> normal wake + check
    BTN_GESTURE_REFRESH,    // held >= BTN_REFRESH_HOLD_MS, released before provisioning
    BTN_GESTURE_PROVISION,  // held >= PROVISION_HOLD_MS -> enter captive portal
} btn_gesture_t;

// Run the boot-hold window: block while the button is held at boot (keeping USB
// enumerated for re-flashing), then classify the gesture by how long it was held.
// A held-to-20s returns PROVISION; a released 3-20s hold returns REFRESH; a quick
// tap / no hold returns NONE.
btn_gesture_t power_boot_gesture(void);

void power_deep_sleep(uint32_t seconds);   // timer + button wake, then sleep (no return)
