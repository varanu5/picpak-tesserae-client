// main.c — PicPak custom firmware: M2 Tesserae REST wake loop.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "config_store.h"
#include "defaults.h"
#include "board.h"
#include "power.h"
#include "epd_driver.h"
#include "wifi_manager.h"
#include "rest_handler.h"
#include "provisioning.h"
#include "splash.h"
#include "lowbatt.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "picpak";

// On this board, a WiFi-time voltage sag shows up as a brownout OR (with the
// brownout threshold lowered) as an interrupt/task watchdog reset. Treat all of
// them as "power too low right now -> back off and let the battery recover".
static bool is_power_fault_reset(esp_reset_reason_t r) {
    return r == ESP_RST_BROWNOUT || r == ESP_RST_INT_WDT ||
           r == ESP_RST_WDT      || r == ESP_RST_TASK_WDT;
}

void app_main(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "PicPak custom fw %s boot (panel %dx%d, wake=%d)",
             FW_VERSION, EPD_W, EPD_H, (int)reason);

    ESP_ERROR_CHECK(config_init());

    // Provisioning: a 20s button-hold at wake, or no usable WiFi SSID, enters the
    // captive portal. (power_boot_gesture also runs the shorter flash-hold window.)
    bool want_provision = power_boot_gesture();
    if (!want_provision) {
        char ssid[33], pass[65];
        want_provision = !config_get_wifi(ssid, sizeof ssid, pass, sizeof pass);
    }
    if (want_provision) {
        splash_show_setup();                           // panel shows AP name/password while you provision
        if (provisioning_run_blocking() == ESP_OK) {
            esp_restart();                             // saved -> re-enter normal path with new creds
        }
        power_deep_sleep(SLEEP_INTERVAL_DEFAULT_S);   // timeout: sleep, retry portal next wake
    }

    // Brownout-aware boot: if we reset from a brownout, the battery is too low to
    // safely run the WiFi radio. Defer and deep-sleep so it can recover instead of
    // rapid reset-looping (which drains faster than it charges).
    if (is_power_fault_reset(reason)) {
        ESP_LOGW(TAG, "power-fault wake (reason=%d): deferring %d s to let battery recover",
                 (int)reason, BROWNOUT_RECOVERY_SLEEP_S);
        power_deep_sleep(BROWNOUT_RECOVERY_SLEEP_S);   // no return
    }

    // Low-battery gate. Measure now: the button (shared with the battery ADC on GPIO2) has been
    // released by power_boot_gesture, so GPIO2 reads the cell cleanly — and it's still before
    // WiFi/EPD load the rail. Below ~0% (3300 mV, bottom of the battpct.h curve) -> charge screen
    // + low-power poll until the voltage rises
    // (charging). A button-wake resumes NORMAL so the device is always recoverable.
    power_measure_battery();
    switch (lowbatt_gate(power_battery_mv(), esp_sleep_get_wakeup_cause())) {
        case LOWBATT_ARM:
            ESP_LOGW(TAG, "battery low: charge screen + %lu s low-power poll",
                     (unsigned long)lowbatt_wake_s());
            splash_show_lowbatt();
            power_deep_sleep(lowbatt_wake_s());     // no return
            break;
        case LOWBATT_STAY_LOW:
            ESP_LOGW(TAG, "battery still low: %lu s low-power poll", (unsigned long)lowbatt_wake_s());
            power_deep_sleep(lowbatt_wake_s());     // no return
            break;
        case LOWBATT_NORMAL:
        default:
            break;   // healthy / recovered / button-wake -> normal cycle
    }

    // One-shot after provisioning: paint the "waiting for first frame" splash and
    // clear the ETag so the first /frame returns 200 and the real photo repaints
    // over the splash. Deliberately after the power-fault and low-battery gates:
    // the 13-22 s EPD refresh is the heaviest rail load we have, and taking the
    // flag earlier would consume it just before a brownout could kill the paint —
    // gated here, the splash intent survives to the next healthy boot.
    if (config_take_paired_pending()) {
        config_set_etag("");
        splash_show_paired();
        ESP_LOGI(TAG, "paired_pend: painted paired splash + cleared ETag");
    }

    // No NTP here: nothing in this build reads the clock, and the C3 keeps RTC
    // time across deep sleep (the reference's every-wake SNTP works around its
    // AXP2101 PMIC corrupting the RTC — hardware we don't have). wifi_sync_ntp
    // stays compiled: it becomes the cold-boot clock source in MQTT mode (M5),
    // and `server_time` seeding covers REST when sleep_until lands.
    int next = SLEEP_INTERVAL_DEFAULT_S;
    vTaskDelay(pdMS_TO_TICKS(WIFI_SETTLE_MS));   // let the rail settle before the radio
    if (wifi_start_sta() == ESP_OK) {
        next = rest_run_loop(reason);  // network I/O only; a new frame is buffered, not painted
    } else {
        ESP_LOGW(TAG, "WiFi failed; keeping last image, retry next wake");
    }
    wifi_stop();

    // Transport-agnostic contract: all network I/O finishes, then radio off, then
    // paint. Radio + EPD refresh together is the worst-case rail load on a board
    // that browns out on radio spikes, and the radio idling through a 13-22 s
    // refresh burns ~80 mA for nothing. EPD init is lazy for the same reason:
    // most wakes end in a 304 and the panel never powers up at all.
    const uint8_t *fb = rest_pending_frame();
    if (fb) {
        if (epd_init() == ESP_OK) {
            ESP_LOGI(TAG, "painting new frame (radio off)");
            epd_display(fb);
            rest_frame_painted();   // persist the ETag only after a successful paint
            epd_sleep();
        } else {
            ESP_LOGW(TAG, "epd_init failed; keeping last image, retry next wake");
        }
    }
    power_deep_sleep((uint32_t)next);   // no return
}
