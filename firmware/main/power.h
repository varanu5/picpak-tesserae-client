// power.h — battery ADC, deep sleep, boot button window.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdbool.h>

void power_measure_battery(void);  // read + cache once (call early, before WiFi/EPD load the rail)
int  power_battery_mv(void);   // cached Li-Po mV (measures on first call if not yet cached)
int  power_battery_pct(int mv); // 0..100 via a Li-Po discharge curve (pass mv from power_battery_mv)

bool power_button_held(void);  // GPIO2 currently pressed (active-low)

// Run the boot-hold window: block while the button is held at boot (keeping USB
// enumerated for re-flashing). Returns true if held >= PROVISION_HOLD_MS (caller
// should enter provisioning); false otherwise (normal boot / short flash-hold).
bool power_boot_gesture(void);

void power_deep_sleep(uint32_t seconds);   // timer + button wake, then sleep (no return)
