// power.c — battery ADC, deep sleep, boot button window.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "power.h"
#include "board.h"
#include "defaults.h"
#include "battpct.h"

#include <stdlib.h>   // qsort
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pwr";

static int cmp_int(const void *a, const void *b) {
    return (*(const int *)a) - (*(const int *)b);
}

// Read the battery via ADC1_CH2 (GPIO2), curve-fitting calibrated, median of
// samples — mirrors the stock "5x20 median" robustness. voltageMv =
// calibrated pin mV * BATT_DIVIDER (the board's resistor divider).
static int power_read_mv(void) {
    adc_oneshot_unit_handle_t adc = NULL;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) return -1;   // failure sentinel (0 would read as a flat cell)
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(adc, BATT_ADC_CHANNEL, &ccfg);

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
        adc_cali_raw_to_voltage(cali, raw_med, &pin_mv);
        adc_cali_delete_scheme_curve_fitting(cali);
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
    if (!power_button_held()) return BTN_GESTURE_NONE;
    ESP_LOGW(TAG, "button held at boot: flash window %d ms (refresh at %d ms, provisioning at %d ms)",
             BOOT_HOLD_WINDOW_MS, BTN_REFRESH_HOLD_MS, PROVISION_HOLD_MS);
    int waited = 0;
    while (power_button_held()) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
        if (waited >= PROVISION_HOLD_MS) {
            ESP_LOGW(TAG, "held %d ms -> entering provisioning", waited);
            return BTN_GESTURE_PROVISION;
        }
        if (waited >= BOOT_HOLD_WINDOW_MS && waited % 1000 == 0)
            ESP_LOGW(TAG, "still holding (%d ms)... keep holding to %d for provisioning",
                     waited, PROVISION_HOLD_MS);
    }
    // Released before the provisioning threshold. A deliberate >= 3 s hold is a
    // refresh request; a quick tap is just a normal wake-and-check. Classified
    // here on RELEASE, so a continuous hold to 20 s hits provisioning above and
    // never trips a refresh on its way there.
    if (waited >= BTN_REFRESH_HOLD_MS) {
        ESP_LOGW(TAG, "held %d ms -> refresh request", waited);
        return BTN_GESTURE_REFRESH;
    }
    return BTN_GESTURE_NONE;
}

void power_deep_sleep(uint32_t seconds) {
    if (seconds < SLEEP_INTERVAL_MIN_S) seconds = SLEEP_INTERVAL_MIN_S;
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

    ESP_LOGI(TAG, "deep sleep %u s (button wake armed)", (unsigned)seconds);
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_BTN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}
