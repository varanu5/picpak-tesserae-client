// lowbatt_core.h — pure low-battery gate decision (host-testable; no hardware/NVS).
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// One decision per wake. Below ARM (after a short debounce streak) the device locks into a
// low-power poll; while locked it stays low until the cell recovers — either absolutely
// (>= CLEAR) or by climbing RISE mV above its lowest-seen baseline (a rising cell means a
// charger is attached; the C3 has no VBUS line to sense that directly). A button or USB wake
// is a deliberate "resume" and never gates. State is caller-owned (kept in RTC-RAM), so this
// stays a pure function and is unit-testable on the host.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Readings below this are implausible for a Li-Po (no cell, button shorting the shared ADC
// pin, or an ADC-failure sentinel like -1/0) — never gate on them.
#define LOWBATT_MIN_PLAUSIBLE_MV 2500

typedef struct {
    uint16_t arm_mv;      // lock when the cell is below this
    uint16_t clr_mv;      // unlock at/above this (absolute recovery)
    uint16_t rise_mv;     // unlock if the cell climbs this far above baseline (charging)
    uint8_t  arm_streak;  // consecutive sub-ARM reads needed to lock (debounce)
} lowbatt_cfg_t;

typedef struct {
    bool    lock;         // currently in the low-power poll
    int16_t last_mv;      // baseline while locked (running high-water mark); <0 = unset
    uint8_t low_streak;   // consecutive sub-ARM reads while still unlocked
} lowbatt_state_t;

typedef enum {
    LOWBATT_NORMAL = 0,   // run the normal wake cycle
    LOWBATT_ARM,          // just locked: paint the charge screen once, then poll
    LOWBATT_STAY_LOW,     // still locked: poll again without repainting
} lowbatt_action_t;

typedef struct {
    lowbatt_action_t action;
    lowbatt_state_t  next;   // state the caller should persist for the next wake
} lowbatt_result_t;

// Decide the action for this wake and the state to carry forward. Pure: no side effects.
//   batt_mv     below LOWBATT_MIN_PLAUSIBLE_MV means an implausible read (ADC failure, no
//               cell, button shorting the shared ADC pin) — never gate on garbage.
//   button_wake this wake came from the button (a deliberate resume).
//   usb_present tethered to a data host (kept for callers that can detect it; false is fine).
//   enabled     master switch; off -> always NORMAL, state untouched.
static inline lowbatt_result_t lowbatt_decide(int batt_mv, bool button_wake, bool usb_present,
                                              bool enabled, lowbatt_state_t st, lowbatt_cfg_t cfg) {
    lowbatt_result_t r = { LOWBATT_NORMAL, st };

    if (!enabled) return r;                       // gate off -> behave exactly as before

    if (button_wake || usb_present) {             // deliberate resume / tethered -> never gate
        r.next.lock = false;
        r.next.low_streak = 0;
        if (batt_mv >= LOWBATT_MIN_PLAUSIBLE_MV) r.next.last_mv = (int16_t)batt_mv;
        return r;
    }

    if (batt_mv < LOWBATT_MIN_PLAUSIBLE_MV) return r;   // implausible reading -> don't act on it

    if (st.lock) {                                // in the low-power poll: look for recovery
        bool rose = (st.last_mv >= 0) && (batt_mv - (int)st.last_mv >= (int)cfg.rise_mv);
        if (batt_mv >= (int)cfg.clr_mv || rose) {
            r.next.lock = false;                  // recovered (absolute or charging) -> resume
            r.next.low_streak = 0;
            r.next.last_mv = (int16_t)batt_mv;
            return r;                             // NORMAL
        }
        if (batt_mv > st.last_mv) r.next.last_mv = (int16_t)batt_mv;   // track the high-water mark
        r.action = LOWBATT_STAY_LOW;
        return r;
    }

    if (batt_mv >= (int)cfg.arm_mv) {             // healthy read -> clear the debounce
        r.next.low_streak = 0;
        return r;                                 // NORMAL
    }

    // Below ARM and not yet locked: count consecutive lows, lock once the streak is met. The
    // streak rejects a single noisy dip / a momentary load sag.
    uint8_t streak = (st.low_streak < 255) ? (uint8_t)(st.low_streak + 1) : 255;
    r.next.low_streak = streak;
    if (streak >= cfg.arm_streak) {
        r.next.lock = true;
        r.next.last_mv = (int16_t)batt_mv;        // baseline for the rise check
        r.next.low_streak = 0;
        r.action = LOWBATT_ARM;
    }
    return r;                                     // streak not reached yet -> NORMAL
}
