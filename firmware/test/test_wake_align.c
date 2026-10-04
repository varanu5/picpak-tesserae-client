// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "wake_align.h"

#define EPOCH 1800000000U
#define US 1000000LL

static int64_t now;
static int set_error;

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    tv->tv_sec = (time_t)(now / US);
    tv->tv_usec = (int)(now % US);
    return 0;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz) {
    (void)tz;
    if (set_error) return set_error;
    now = (int64_t)tv->tv_sec * US + tv->tv_usec;
    return 0;
}

static void cold_start(void) {
    set_error = 0;
    now = 0;
    wake_align_begin(false, false);
    wake_align_sync(EPOCH);
}

static void expect_timer(uint32_t fallback, int64_t expected) {
    int64_t actual = (int64_t)wake_align_timer_us(fallback);
    assert(llabs(actual - expected) <= 1);
}

static void learn_drift(int local_elapsed) {
    cold_start();
    wake_align_set_target(0);
    expect_timer(900, 900 * US);
    wake_align_begin(true, true);
    now = ((int64_t)EPOCH + local_elapsed) * US;
    wake_align_sync(EPOCH + 900);
    wake_align_set_target(0);
}

static void test_epochs(void) {
    assert(wake_align_epoch(EPOCH) == EPOCH);
    assert(wake_align_epoch(2200000000.0) == 2200000000U);
    assert(wake_align_epoch(4102444800.0) == 4102444800U);
    assert(!wake_align_epoch(0));
    assert(!wake_align_epoch(-1));
    assert(!wake_align_epoch(1700000000));
    assert(!wake_align_epoch(EPOCH + 0.5));
    assert(!wake_align_epoch(4102444801.0));
    assert(!wake_align_epoch(1e99));
    assert(!wake_align_epoch(NAN));
    assert(!wake_align_epoch(INFINITY));
}

static void test_targets(void) {
    cold_start();
    wake_align_set_target(EPOCH + 900);
    now += 20500000;
    expect_timer(900, 879500000);

    wake_align_set_target(0);
    expect_timer(900, 900 * US);
    wake_align_set_target(EPOCH);
    expect_timer(900, 900 * US);
    wake_align_set_target(EPOCH + 50);
    expect_timer(900, 900 * US);
    wake_align_set_target(EPOCH + 2000);
    expect_timer(900, 900 * US);
    wake_align_set_target(EPOCH + 921);
    expect_timer(900, 900 * US);
    wake_align_set_target(EPOCH + 900);
    expect_timer(60, 60 * US);
    now = ((int64_t)EPOCH + 870) * US;
    expect_timer(900, 30 * US);

    // A new wake must not reuse the previous absolute target.
    wake_align_begin(true, true);
    expect_timer(900, 900 * US);
    wake_align_set_target(EPOCH + 950);
    expect_timer(900, 900 * US);
    now = 0;
    expect_timer(900, 900 * US);

    cold_start();
    wake_align_sync(2200000000U);
    wake_align_set_target(2200000900U);
    now += 20 * US;
    expect_timer(900, 880 * US);
}

static void test_drift(void) {
    // A slow clock needs a shorter timer. A fast clock needs a longer one.
    learn_drift(891);
    expect_timer(900, 891 * US);
    wake_align_set_target(EPOCH + 1800);
    now += 20 * US;
    expect_timer(900, 871200000);

    // A second response during the same wake only updates the time baseline.
    wake_align_sync(EPOCH + 960);
    wake_align_set_target(0);
    expect_timer(900, 891 * US);
    wake_align_begin(true, true);
    now = ((int64_t)EPOCH + 1842) * US;
    wake_align_sync(EPOCH + 1860);
    wake_align_set_target(0);
    expect_timer(900, 888300000);

    learn_drift(909);
    expect_timer(900, 909 * US);
    learn_drift(900);
    expect_timer(900, 900 * US);

    // Cold resets discard calibration.
    learn_drift(891);
    wake_align_begin(false, false);
    wake_align_sync(EPOCH + 1000);
    wake_align_set_target(0);
    expect_timer(900, 900 * US);
}

static void test_exclusions(void) {
    // A button can interrupt sleep at any time. It must not train the estimate.
    learn_drift(891);
    expect_timer(900, 891 * US);
    wake_align_begin(true, false);
    now += 80 * US;
    wake_align_sync(EPOCH + 1000);
    wake_align_set_target(0);
    expect_timer(900, 891 * US);

    // Without an accepted REST status, normal sleeps remain uncorrected.
    wake_align_begin(true, true);
    now = ((int64_t)EPOCH + 1891) * US;
    wake_align_sync(EPOCH + 1900);
    expect_timer(180, 180 * US);
    wake_align_begin(true, true);
    now += 200 * US;
    wake_align_sync(EPOCH + 2800);
    wake_align_set_target(0);
    expect_timer(900, 891 * US);

    // Short baselines and large clock jumps are not useful drift samples.
    cold_start();
    wake_align_set_target(0);
    expect_timer(30, 30 * US);
    wake_align_begin(true, true);
    now += 29 * US;
    wake_align_sync(EPOCH + 30);
    wake_align_set_target(0);
    expect_timer(900, 900 * US);
    learn_drift(700);
    expect_timer(900, 900 * US);
    learn_drift(840);
    expect_timer(900, 900 * US);
    learn_drift(960);
    expect_timer(900, 900 * US);

    cold_start();
    wake_align_set_target(0);
    expect_timer(900, 900 * US);
    wake_align_begin(true, true);
    now += 900 * US;
    wake_align_sync(EPOCH - 100);
    wake_align_set_target(0);
    expect_timer(900, 900 * US);

    cold_start();
    wake_align_begin(true, true);
    set_error = -1;
    wake_align_sync(EPOCH + 900);
    wake_align_set_target(EPOCH + 1000);
    expect_timer(900, 900 * US);
    set_error = 0;
}

int main(void) {
    test_epochs();
    test_targets();
    test_drift();
    test_exclusions();
    puts("test_wake_align: ok");
    return 0;
}
