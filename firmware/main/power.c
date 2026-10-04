// power.c — battery ADC, deep sleep, boot button window.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "power.h"
#include "board.h"
#include "defaults.h"
#include "battpct.h"
#include "led.h"
#include "wake_align.h"

#include <stdlib.h>   // qsort
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pwr";

static int cmp_int(const void *a, const void *b) {
    return (*(const int *)a) - (*(const int *)b);
}

// Read the battery via ADC1_CH2 (GPIO2), curve-fitting calibrated, median of
// samples for robustness. voltageMv =
// calibrated pin mV * BATT_DIVIDER (the board's resistor divider).
static int power_read_mv(void) {
    adc_oneshot_unit_handle_t adc = NULL;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) return -1;   // failure sentinel (0 would read as a flat cell)
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(adc, BATT_ADC_CHANNEL, &ccfg) != ESP_OK) {
        adc_oneshot_del_unit(adc);
        return -1;
    }

    enum { N = 20 };
    int s[N], got = 0;
    for (int i = 0; i < N; i++) {
        int r;
        if (adc_oneshot_read(adc, BATT_ADC_CHANNEL, &r) == ESP_OK) s[got++] = r;
    }
    if (!got) { adc_oneshot_del_unit(adc); return -1; }
    qsort(s, got, sizeof(int), cmp_int);
    int raw_med = s[got / 2];

    // Curve-fitting calibration corrects the C3 ADC's nonlinearity -> accurate pin mV.
    int pin_mv = 0;
    adc_cali_handle_t cali = NULL;
    adc_cali_curve_fitting_config_t calcfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = BATT_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&calcfg, &cali) == ESP_OK) {
        esp_err_t err = adc_cali_raw_to_voltage(cali, raw_med, &pin_mv);
        adc_cali_delete_scheme_curve_fitting(cali);
        if (err != ESP_OK) {
            adc_oneshot_del_unit(adc);
            return -1;
        }
    } else {
        pin_mv = (int)((raw_med / 4095.0f) * 3100.0f);   // fallback: crude linear
    }
    adc_oneshot_del_unit(adc);

    int mv = (int)(pin_mv * BATT_DIVIDER);
    ESP_LOGI(TAG, "battery: raw_med=%d pin_mv=%d -> mv=%d (BATT_DIVIDER=%.3f)",
             raw_med, pin_mv, mv, (double)BATT_DIVIDER);
    return mv;
}

static int s_batt_mv = -1;   // cached reading; <0 = not yet measured this boot (or read failed -> retry)

// Measure once and cache — call early (before WiFi/EPD load the rail) for an accurate value.
void power_measure_battery(void) { s_batt_mv = power_read_mv(); }

int power_battery_mv(void) {
    if (s_batt_mv < 0) s_batt_mv = power_read_mv();
    return s_batt_mv;
}

// mV -> % via the PhotoPainter Li-Po discharge curve (see battpct.h — granular high-end).
int power_battery_pct(int mv) { return battpct(mv); }

bool power_button_held(void) {
    gpio_set_direction(PIN_BTN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BTN, GPIO_PULLUP_ONLY);
    return gpio_get_level(PIN_BTN) == 0;   // active-low
}

btn_gesture_t power_boot_gesture(void) {
    if (!power_button_held()) return BTN_GESTURE_NONE;   // timer wake, or a tap already released
    // Button is held. The one-blink wake acknowledge already fired in app_main;
    // add the hold progression as screen-free feedback of how long it is held:
    // off (<5 s) -> steady on (refresh at 5 s) -> pulsing (BLE at 10 s) -> burst
    // (provisioning armed at 20 s).
    ESP_LOGW(TAG, "button held at boot: flash window %d ms (refresh at %d ms, maintenance at %d ms, provisioning at %d ms)",
             BOOT_HOLD_WINDOW_MS, BTN_REFRESH_HOLD_MS, BTN_MAINTENANCE_HOLD_MS, PROVISION_HOLD_MS);
    int waited = 0;
    int64_t started = esp_timer_get_time();
    int last_log = 0;
    while (power_button_held()) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited = (int)((esp_timer_get_time() - started) / 1000);
        if (waited >= PROVISION_HOLD_MS) {
            // Provisioning armed: rapid confirm burst so the user can release now.
            for (int i = 0; i < 6; i++) { led_set((i % 2) == 0); vTaskDelay(pdMS_TO_TICKS(60)); }
            led_set(false);
            ESP_LOGW(TAG, "held %d ms -> entering provisioning", waited);
            return BTN_GESTURE_PROVISION;
        }
        // LED cue painted by zone each tick: pulsing = release now for BLE
        // maintenance (>=10 s); steady on = refresh armed (>=5 s); off below 5 s.
        if (waited >= BTN_MAINTENANCE_HOLD_MS)
            led_set(((waited - BTN_MAINTENANCE_HOLD_MS) / 200) % 2 == 0); // release for BLE
        else if (waited >= BTN_REFRESH_HOLD_MS)
            led_set(true);   // steady on = refresh armed; keep holding to 10 s for BLE
        else
            led_set(false);
        // Periodic "still holding" heartbeat once past the flash window. waited is
        // timer-derived and steps ~200 ms, so log on elapsed delta, not waited % 1000.
        if (waited >= BOOT_HOLD_WINDOW_MS && waited - last_log >= 1000) {
            last_log = waited;
            ESP_LOGW(TAG, "still holding (%d ms)... keep holding to %d for provisioning",
                     waited, PROVISION_HOLD_MS);
        }
    }
    led_set(false);
    // Released before the provisioning threshold. A deliberate >= 5 s hold is a
    // refresh request. Classified here on RELEASE, so a continuous hold to 20 s
    // hits provisioning above and never trips a refresh on its way there.
    btn_gesture_t gesture = button_release_gesture((uint32_t)waited);
    if (gesture != BTN_GESTURE_TAP) return gesture;
    // Held at boot but released before 5 s: a deliberate short press (tap). A
    // human tap holds the pin low ~50-150 ms, long enough to have read as held
    // at boot, so electrical noise on GPIO2 (shared with the battery ADC) can't
    // fake it. Maps to a deck next-page nav in the REST loop.
    ESP_LOGW(TAG, "held %d ms -> tap (deck next)", waited);
    return BTN_GESTURE_TAP;
}

static void enter_deep_sleep(uint32_t seconds, bool scheduled) {
    // Zero is the manual photo mode: GPIO wake only, no periodic radio work.
    if (seconds > SLEEP_INTERVAL_MAX_S) seconds = SLEEP_INTERVAL_MAX_S;

    // The wake button (GPIO2) is shared with the battery ADC. A battery read
    // leaves the pin in analog mode, and a held/bouncing press would immediately
    // re-trigger the active-low wake. Restore a clean pulled-up digital input and
    // wait (bounded) for the button to be released before arming the wake source.
    gpio_reset_pin(PIN_BTN);
    gpio_set_direction(PIN_BTN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BTN, GPIO_PULLUP_ONLY);
    for (int waited = 0; gpio_get_level(PIN_BTN) == 0 && waited < 3000; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    uint64_t timer_us = (uint64_t)seconds * 1000000ULL;
    if (scheduled) timer_us = wake_align_timer_us(seconds);
    ESP_LOGI(TAG, "deep sleep %.3f s (button wake armed)", (double)timer_us / 1000000.0);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (timer_us) esp_sleep_enable_timer_wakeup(timer_us);
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_BTN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

void power_sleep_until_button(void) { enter_deep_sleep(0, false); }
void power_deep_sleep(uint32_t seconds) {
    if (seconds < SLEEP_INTERVAL_MIN_S) seconds = SLEEP_INTERVAL_MIN_S;
    enter_deep_sleep(seconds, false);
}

void power_scheduled_sleep(uint32_t seconds) {
    if (seconds < SLEEP_INTERVAL_MIN_S) seconds = SLEEP_INTERVAL_MIN_S;
    enter_deep_sleep(seconds, true);
}
