// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "wake_align.h"
#include "defaults.h"

#include <math.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "wake_align";

#define EPOCH_MAX 4102444800ULL
#define MIN_BASELINE_US 60000000LL
#define MAX_CLOCK_ERROR_US 120000000LL
#define MAX_DRIFT_PPM 60000.0

RTC_DATA_ATTR static int64_t s_last_sync_us;
RTC_DATA_ATTR static double s_drift_ppm;
RTC_DATA_ATTR static bool s_drift_valid;
RTC_DATA_ATTR static bool s_previous_rest_sleep;

static uint32_t s_target;
static bool s_schedule_ready;
static bool s_synced;
static bool s_learn;

static int64_t clock_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static bool sane_clock(int64_t us) {
    return us >= (int64_t)CLOCK_SANE_EPOCH * 1000000LL &&
           us <= (int64_t)EPOCH_MAX * 1000000LL;
}

void wake_align_begin(bool deep_sleep_reset, bool timer_wake) {
    if (!deep_sleep_reset) {
        s_last_sync_us = 0;
        s_drift_ppm = 0;
        s_drift_valid = false;
        s_previous_rest_sleep = false;
    }
    s_learn = timer_wake && s_previous_rest_sleep;
    s_previous_rest_sleep = false;
    s_target = 0;
    s_schedule_ready = false;
    s_synced = false;
}

uint32_t wake_align_epoch(double value) {
    if (!isfinite(value) || value < CLOCK_SANE_EPOCH || value > EPOCH_MAX ||
        value != floor(value)) return 0;
    return (uint32_t)value;
}

void wake_align_sync(uint32_t epoch) {
    if (!wake_align_epoch(epoch)) return;
    int64_t server_us = (int64_t)epoch * 1000000LL;
    int64_t local_us = clock_us();
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        s_learn = false;
        ESP_LOGW(TAG, "server clock update failed");
        return;
    }

    // Only the first clock update after a REST timer sleep measures drift.
    if (s_learn && s_last_sync_us && sane_clock(local_us)) {
        int64_t elapsed = server_us - s_last_sync_us;
        int64_t error = server_us - local_us;
        if (elapsed >= MIN_BASELINE_US &&
            error > -MAX_CLOCK_ERROR_US && error < MAX_CLOCK_ERROR_US) {
            double ppm = (double)error * 1000000.0 / (double)elapsed;
            if (fabs(ppm) < MAX_DRIFT_PPM) {
                s_drift_ppm = s_drift_valid ? s_drift_ppm * 0.7 + ppm * 0.3 : ppm;
                s_drift_valid = true;
                ESP_LOGI(TAG, "clock drift sample %+.0f ppm, estimate %+.0f ppm",
                         ppm, s_drift_ppm);
            } else {
                s_drift_valid = false;
            }
        } else if (elapsed < 0 || error <= -MAX_CLOCK_ERROR_US ||
                   error >= MAX_CLOCK_ERROR_US) {
            s_drift_valid = false;
        }
    }
    s_learn = false;
    s_synced = true;
    s_last_sync_us = server_us;
}

void wake_align_set_target(uint32_t epoch) {
    s_target = wake_align_epoch(epoch);
    s_schedule_ready = true;
}

uint64_t wake_align_timer_us(uint32_t fallback_s) {
    uint64_t fallback_us = (uint64_t)fallback_s * 1000000ULL;
    if (!s_schedule_ready || !s_synced) return fallback_us;

    int64_t now = clock_us();
    if (!sane_clock(now)) return fallback_us;
    uint64_t sleep_us = fallback_us;
    if (s_target) {
        int64_t remaining = (int64_t)s_target * 1000000LL - now;
        if (remaining >= (int64_t)SLEEP_INTERVAL_MIN_S * 1000000LL &&
            remaining <= (int64_t)fallback_us + 2000000LL) {
            if ((uint64_t)remaining < sleep_us) sleep_us = (uint64_t)remaining;
            ESP_LOGI(TAG, "absolute wake %lu, sleep %.3f s, relative %lu s",
                     (unsigned long)s_target, (double)sleep_us / 1000000.0,
                     (unsigned long)fallback_s);
        } else {
            ESP_LOGI(TAG, "absolute wake unavailable, using relative interval");
        }
    }

    s_previous_rest_sleep = true;
    if (s_drift_valid) {
        double factor = 1.0 - s_drift_ppm / 1000000.0;
        if (factor < 0.94) factor = 0.94;
        if (factor > 1.06) factor = 1.06;
        sleep_us = (uint64_t)((double)sleep_us * factor);
        ESP_LOGI(TAG, "corrected timer %.3f s", (double)sleep_us / 1000000.0);
    }
    return sleep_us;
}
