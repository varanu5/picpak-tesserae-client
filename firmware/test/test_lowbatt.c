// test_lowbatt.c — host unit test for the pure low-battery gate FSM (lowbatt_core.h).
// cc firmware/test/test_lowbatt.c -Ifirmware/main -o /tmp/t && /tmp/t
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include "lowbatt_core.h"

static const lowbatt_cfg_t CFG = { .arm_mv = 3300, .clr_mv = 3500, .rise_mv = 40, .arm_streak = 2 };
static const lowbatt_state_t FRESH = { .lock = false, .last_mv = -1, .low_streak = 0 };

// helper: decide with the gate enabled + default cfg
static lowbatt_result_t d(int mv, bool btn, bool usb, lowbatt_state_t st) {
    return lowbatt_decide(mv, btn, usb, true, st, CFG);
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

    // button/usb wake bypasses the gate even when locked + low
    r = d(3100, true, false, locked);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock);
    r = d(3100, false, true, locked);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock);

    // implausible read never gates, state untouched
    r = d(-1, false, false, FRESH);
    assert(r.action == LOWBATT_NORMAL && !r.next.lock && r.next.low_streak == 0);

    printf("PASS\n");
    return 0;
}
