// lowbatt.h — RTC/hardware glue around the pure FSM in lowbatt_core.h.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdint.h>
#include "esp_sleep.h"
#include "lowbatt_core.h"

// Fixed thresholds (shipped constant — no NVS/console on this build).
#define LOWBATT_ARM_MV   3300   // ~10%
#define LOWBATT_CLR_MV   3500   // ~30%, hysteresis
#define LOWBATT_RISE_MV  40     // "charging" if mV rose >= this over the poll window
#define LOWBATT_STREAK   2      // consecutive sub-ARM reads before arming
#define LOWBATT_WAKE_S   900    // 15-min low-power poll

// Run the gate for this wake. ARM -> caller paints the charge screen once + deep-sleeps
// lowbatt_wake_s(); STAY_LOW -> deep-sleep without a repaint; NORMAL -> fall through.
lowbatt_action_t lowbatt_gate(int batt_mv, esp_sleep_wakeup_cause_t cause);
uint32_t lowbatt_wake_s(void);
