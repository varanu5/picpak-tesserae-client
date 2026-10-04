// test_lowbatt.c — host unit test for the pure low-battery gate FSM (lowbatt_core.h).
// cc firmware/test/test_lowbatt.c -Ifirmware/main -o /tmp/t && /tmp/t
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <stdio.h>
#include "lowbatt_core.h"

// Representative config for exercising the FSM — not the shipped policy (see lowbatt.h for that;
// it can't be included here, it pulls in esp_sleep.h). The logic under test is threshold-agnostic.
static const lowbatt_cfg_t CFG = { .arm_mv = 3300, .clr_mv = 3500, .rise_mv = 40, .arm_streak = 2 };
static const lowbatt_state_t FRESH = { .lock = false, .last_mv = -1, .low_streak = 0 };

// helper: decide with the gate enabled + default cfg
static lowbatt_result_t d(int mv, bool force, bool usb, lowbatt_state_t st) {
    return lowbatt_decide(mv, force, usb, true, st, CFG);
}

int main(void) {
    lowbatt_result_t r;

    // disabled -> always NORMAL, even flat
    r = lowbatt_decide(3000, false, false, false, FRESH, CFG);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock);

    // healthy -> NORMAL, streak cleared
    r = d(3900, false, false, FRESH);
    assert(r.action == LOWBATT_NORMAL && r.next.low_streak == 0);

    // a single sub-ARM read does NOT arm (debounce)
    r = d(3250, false, false, FRESH);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.low_streak == 1);

    // second consecutive low -> ARM, baseline captured
    r = d(3230, false, false, r.next);
    assert(r.action == LOWBATT_ARM && r.next.lock && r.next.last_mv == 3230 && r.next.low_streak == 0);

    // a healthy read between two lows resets the streak (no arming)
    lowbatt_result_t one_low = d(3250, false, false, FRESH);
    r = d(3900, false, false, one_low.next);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.low_streak == 0);

    // locked + still low -> STAY_LOW; high-water mark climbs but stays below rise
    lowbatt_state_t locked = { .lock = true, .last_mv = 3230, .low_streak = 0 };
    r = d(3220, false, false, locked);            // sagged below baseline
    assert(r.action == LOWBATT_STAY_LOW && r.next.last_mv == 3230);
    r = d(3260, false, false, locked);            // up 30 < rise(40): high-water rises, still low
    assert(r.action == LOWBATT_STAY_LOW && r.next.last_mv == 3260);

    // relative recovery: +40 above baseline -> NORMAL, unlock
    r = d(3270, false, false, locked);            // 3270 - 3230 = 40 >= rise
    assert(r.action == LOWBATT_NORMAL && !r.next.lock);

    // absolute recovery: at/above CLEAR -> NORMAL even without a big step
    r = d(3520, false, false, locked);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock);

    // force-resume (5 s hold) or USB wake overrides the gate even when locked + low, and
    // pre-loads the debounce so a single still-low next read re-arms (no wasted extra fetch)
    r = d(3100, /*force*/true, false, locked);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.low_streak == CFG.arm_streak - 1);
    r = d(3100, false, /*usb*/true, locked);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.low_streak == CFG.arm_streak - 1);

    // prompt re-arm: after a still-low override, one more low read arms immediately
    lowbatt_result_t ov = d(3100, /*force*/true, false, locked);   // low_streak now arm_streak-1
    r = d(3100, false, false, ov.next);                            // single low read
    assert(r.action == LOWBATT_ARM && r.next.lock);

    // force-resume while NOT locked -> NORMAL, no preload (unchanged normal-state behavior)
    r = d(3100, /*force*/true, false, FRESH);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.low_streak == 0);

    // implausible read never gates, state untouched
    r = d(-1, false, false, FRESH);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.low_streak == 0);

    // 0 mV (an ADC-failure sentinel) is implausible too — a Li-Po can't be there
    r = d(0, false, false, FRESH);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.low_streak == 0);

    // anything below the plausibility floor is ignored, not treated as "extremely low"
    r = d(2400, false, false, FRESH);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.low_streak == 0);

    // an implausible read mid-streak neither extends nor resets the debounce
    r = d(0, false, false, one_low.next);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.low_streak == 1);

    // locked + implausible read -> no false recovery, lock persists
    r = d(0, false, false, locked);
    assert(r.action == LOWBATT_STAY_LOW && r.next.lock && r.next.last_mv == 3230);
    r = d(-1, false, false, locked);
    assert(r.action == LOWBATT_STAY_LOW && r.next.lock);
    r = d(2499, false, false, locked);
    assert(r.action == LOWBATT_STAY_LOW && r.next.lock);

    // button wake with an implausible read must not corrupt the baseline
    r = d(0, true, false, locked);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.last_mv == 3230);

    printf("PASS\n");
    return 0;
}
