// lowbatt.c — RTC-RAM glue (fixed thresholds; gate ON by default; no USB-SOF in v1).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "lowbatt.h"
#include "esp_attr.h"   // RTC_DATA_ATTR

// Cross-wake state in RTC-RAM: survives deep-sleep, nulled on cold boot -> always boot NORMAL.
RTC_DATA_ATTR static lowbatt_state_t s_lb;

lowbatt_action_t lowbatt_gate(int batt_mv, esp_sleep_wakeup_cause_t cause) {
    lowbatt_cfg_t cfg = {
        .arm_mv = LOWBATT_ARM_MV, .clr_mv = LOWBATT_CLR_MV,
        .rise_mv = LOWBATT_RISE_MV, .arm_streak = LOWBATT_STREAK,
    };
    bool button_wake = (cause == ESP_SLEEP_WAKEUP_GPIO);   // deliberate resume/flash
    // usb_present=false in v1 (no USB-SOF detection); button-wake is the escape hatch.
    lowbatt_result_t r = lowbatt_decide(batt_mv, button_wake, /*usb=*/false, /*enabled=*/true, s_lb, cfg);
    s_lb = r.next;   // persist for the next wake (RTC-RAM)
    return r.action;
}

uint32_t lowbatt_wake_s(void) { return LOWBATT_WAKE_S; }
