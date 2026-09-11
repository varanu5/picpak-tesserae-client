// main.c — PicPak custom firmware: Tesserae wake loop (REST + MQTT).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "config_store.h"
#include "defaults.h"
#include "board.h"
#include "power.h"
#include "epd_driver.h"
#include "wifi_manager.h"
#include "ble_setup.h"
#include "rest_handler.h"
#include "mqtt_handler.h"
#include "relay.h"
#include "provisioning.h"
#include "splash.h"
#include "lowbatt.h"
#include "manual_core.h"
#include "maintenance_screen.h"
#include "framebuf.h"
#include "led.h"

#include <time.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_attr.h"   // RTC_NOINIT_ATTR
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "picpak";

// Monotonic id bumped per button-refresh request, retained across deep sleep so
// the server can dedup one physical press delivered on both /frame and /status.
// RTC_NOINIT is never initialised by startup, so it is only valid after a
// deep-sleep wake -> zeroed on every other reset reason in app_main.
RTC_NOINIT_ATTR static uint32_t s_button_event_seq;

// Authorship, baked into the binary's .rodata (find it with
// `strings picpak-tesserae-client.bin | grep varanu5`) and printed once per
// boot so any serial log identifies the firmware's origin.
static const char k_credit[] =
    "picpak-tesserae-client (c) 2026 varanu5 - https://github.com/varanu5/picpak-tesserae-client";

// On this board, a WiFi-time voltage sag shows up as a brownout OR (with the
// brownout threshold lowered) as an interrupt/task watchdog reset. Treat all of
// them as "power too low right now -> back off and let the battery recover".
static bool is_power_fault_reset(esp_reset_reason_t r) {
    return r == ESP_RST_BROWNOUT || r == ESP_RST_INT_WDT ||
           r == ESP_RST_WDT      || r == ESP_RST_TASK_WDT;
}

void app_main(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "%s", k_credit);
    ESP_LOGI(TAG, "PicPak custom fw %s boot (panel %dx%d, wake=%d)",
             FW_VERSION, EPD_W, EPD_H, (int)reason);

    ESP_ERROR_CHECK(config_init());

    // One clean blink on every wake (timer or button): a screen-free "the frame
    // woke and is working" pulse. A held button then adds the gesture progression
    // in power_boot_gesture(). GPIO21 is a free LED now that the console is on
    // USB-Serial-JTAG (see led.h / sdkconfig).
    led_init();
    led_ack();

    // RTC_NOINIT survives deep sleep but holds garbage after any other start — a
    // true power-up, an esptool/USB reset (how `idf.py flash` and the web flasher
    // end), an esp_restart() from provisioning, or a brownout-adjacent ESP_RST_UNKNOWN.
    // Only a deep-sleep wake is a real continuation of the previous session, so zero
    // the counter on everything else. (ESP_RST_POWERON alone missed the USB/SW cases
    // and could start the seq from a random value that collides with a server-recorded
    // id, silently dropping one button press.)
    if (reason != ESP_RST_DEEPSLEEP) s_button_event_seq = 0;

    // Provisioning: a 20s button-hold at wake, or no usable WiFi SSID, enters the
    // captive portal. A 5-10s hold is a refresh gesture and a 10-20s hold opens
    // BLE maintenance (both classified on release; see power_boot_gesture).
    // (Also runs the flash-hold window.)
    btn_gesture_t gesture = power_boot_gesture();
    bool ble_recovery = config_take_ble_recovery();
    if (gesture == BTN_GESTURE_MAINTENANCE ||
        (ble_recovery && gesture != BTN_GESTURE_PROVISION)) {
        // The button must be released before the shared GPIO2 battery ADC is read.
        // Keep the normal low-voltage protection in front of radio/display load.
        power_measure_battery();
        if (is_power_fault_reset(reason) ||
            lowbatt_gate(power_battery_mv(), false) != LOWBATT_NORMAL) {
            splash_show_lowbatt();
            if (config_screen_is_bluetooth()) power_sleep_until_button();
            power_deep_sleep(lowbatt_wake_s());
        }
        ble_setup_run(BLE_MAINTENANCE_TIMEOUT_S);
        // Start the normal network cycle with clean Wi-Fi/BLE task ownership.
        // Clear Wi-Fi requests persist a one-shot BLE recovery flag; timeout
        // then falls back to the original AP setup if credentials are still absent.
        esp_restart();
    }
    // Manual (Bluetooth) mode is decided before Wi-Fi credential / AP fallback handling.
    // Unlike the Wi-Fi path there is no server to re-fetch content from, so the low-battery
    // gate must give its own feedback: paint the charge splash when the cell goes low, and
    // clear it to a "ready" screen once it recovers. Wakes are either a button press (a user
    // is present -> advertise for a photo) or a 24 h low-power poll (battery re-check only, so
    // an untouched device still learns it needs charging). A brownout is deferred first,
    // before the gate runs, so a weak cell settles instead of stranding button-only forever.
    if (config_screen_is_bluetooth() && gesture != BTN_GESTURE_PROVISION) {
        if (is_power_fault_reset(reason)) {
            ESP_LOGW(TAG, "manual mode: power-fault wake (reason=%d): deferring %d s",
                     (int)reason, BROWNOUT_RECOVERY_SLEEP_S);
            power_deep_sleep(BROWNOUT_RECOVERY_SLEEP_S);   // no return
        }
        bool user_wake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO ||
                         gesture == BTN_GESTURE_TAP || gesture == BTN_GESTURE_REFRESH;
        power_measure_battery();
        bool was_locked = lowbatt_locked();               // read before the gate mutates state
        lowbatt_action_t gate = lowbatt_gate(power_battery_mv(), false);
        manual_decision_t act = manual_decide(user_wake, gate, was_locked);

        if (act.paint == MANUAL_PAINT_LOWBATT) {
            ESP_LOGW(TAG, "manual mode: battery low -> charge splash + %lu s poll",
                     (unsigned long)lowbatt_wake_s());
            splash_show_lowbatt();
        } else if (act.paint == MANUAL_PAINT_READY) {
            ESP_LOGI(TAG, "manual mode: battery recovered -> ready screen");
            maintenance_screen_photo_ready(framebuf());
            if (epd_present(framebuf()) != ESP_OK)
                ESP_LOGW(TAG, "ready screen failed");
        }
        if (act.run_photo) ble_photo_run(90);
        // Diagnostic summary, printed after the session so it survives the USB-Serial-JTAG
        // re-attach that swallows the early-boot battery log on a deep-sleep wake.
        ESP_LOGI(TAG, "manual mode: battery %d mV, gate=%d, paint=%d, photo=%d",
                 power_battery_mv(), (int)gate, (int)act.paint, (int)act.run_photo);
        power_deep_sleep(lowbatt_wake_s());                // 24 h poll + button wake; no return
    }
    bool want_provision = (gesture == BTN_GESTURE_PROVISION);
    bool want_refresh   = (gesture == BTN_GESTURE_REFRESH);
    bool want_next      = (gesture == BTN_GESTURE_TAP);   // short press -> deck next (REST)
    if (!want_provision) {
        char ssid[33], pass[65];
        want_provision = !config_get_wifi(ssid, sizeof ssid, pass, sizeof pass);
    }
    // A locked (flat) cell can't afford AP-mode radio for a whole setup session, and a brownout
    // mid-provisioning would lose the creds. Refuse the deliberate 20 s re-provision gesture while
    // locked -> show the charge splash instead. Guarded on the gesture only, so a genuinely
    // unprovisioned device (no saved creds -> want_provision) still reaches setup: a fresh/reflashed
    // device is never in the locked state (RTC lock clears on cold boot).
    if (want_provision && gesture == BTN_GESTURE_PROVISION && lowbatt_locked()) {
        ESP_LOGW(TAG, "provision gesture ignored: battery locked, charge first");
        splash_show_lowbatt();
        if (config_screen_is_bluetooth()) power_sleep_until_button();
        power_deep_sleep(lowbatt_wake_s());            // no return
    }
    if (want_provision) {
        splash_show_setup();                           // panel shows AP name/password while you provision
        if (provisioning_run_blocking(NULL) == ESP_OK) {
            ESP_ERROR_CHECK(config_save_screen_mode(false, NULL));
            esp_restart();                             // saved -> re-enter normal path with new creds
        }
        if (config_screen_is_bluetooth()) power_sleep_until_button();
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
    // WiFi/EPD load the rail. At 0% (3400 mV, bottom of the battpct.h curve) -> charge screen
    // + a daily low-power poll until the voltage recovers (charging). A button-wake resumes
    // NORMAL so the device is always recoverable — and is the fast way back after plugging in.
    power_measure_battery();
    bool force_resume = want_refresh;              // a 5 s hold is the deliberate override when locked
    bool was_locked   = lowbatt_locked();          // read before the gate mutates RTC state
    switch (lowbatt_gate(power_battery_mv(), force_resume)) {
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
            break;   // healthy / recovered / override -> normal cycle
    }
    // Just recovered from the locked state via an automatic route (a tap that charged, or the daily
    // poll) — not a 5 s hold. The charge splash was painted outside the ETag machinery, so a plain
    // re-fetch could return 304 (or an unchanged MQTT frame URL, or a relay 304) and skip the paint,
    // stranding the splash on every transport. Clear the frame ref for ALL transports to force a full
    // repaint of the live photo. A 5 s hold (want_refresh) is left alone: its refresh path already
    // drops If-None-Match and pulls fresh content — identical to the good-battery gesture.
    if (was_locked && !want_refresh) config_clear_frame_ref();

    // One-shot after provisioning: paint the "waiting for first frame" splash and
    // clear the frame ref so the first poll repaints the real photo over the splash.
    // Clearing all transports matters on MQTT: a re-provision can leave a retained
    // frame URL identical to the stored one, so a REST-only clear left the paired
    // splash stranded. Deliberately after the power-fault and low-battery gates:
    // the 13-22 s EPD refresh is the heaviest rail load we have, and taking the
    // flag earlier would consume it just before a brownout could kill the paint —
    // gated here, the splash intent survives to the next healthy boot.
    bool just_provisioned = config_take_paired_pending();
    if (just_provisioned) {
        config_clear_frame_ref();
        splash_show_paired();
        ESP_LOGI(TAG, "paired_pend: painted paired splash + cleared frame ref");
    }

    // Transport dispatch: 0 = MQTT, 1 = REST (default; matches the portal's
    // REST-recommended default). Both loops are network-I/O-only — a new frame
    // is buffered, not painted.
    uint8_t transport = config_get_transport(1);
    // Cloud relay is a mutually-exclusive third transport: when configured it
    // wins and the REST/MQTT dispatch below is skipped entirely (same "one
    // transport" rule as the reference — a relay panel has no home server).
    bool use_relay = config_relay_configured();
    int next = SLEEP_INTERVAL_DEFAULT_S;
    bool wifi_ok = false;
    vTaskDelay(pdMS_TO_TICKS(WIFI_SETTLE_MS));   // let the rail settle before the radio
    if (wifi_start_sta() == ESP_OK) {
        wifi_ok = true;
        if (use_relay) {
            // Relay defaults to https, so TLS validity needs a plausible clock —
            // same sanity pattern as the REST https path.
            char rurl[160] = {0};
            config_get_relay_url(rurl, sizeof rurl);
            if (strncmp(rurl, "https://", 8) == 0 && time(NULL) < CLOCK_SANE_EPOCH)
                wifi_sync_ntp();
            // Pairing may span deep sleeps; once paired, run the full relay cycle.
            if (relay_pairing_pending()) {
                switch (relay_pair_step()) {
                    case RELAY_PAIR_DONE:    ESP_LOGI(TAG, "relay pairing complete"); break;
                    case RELAY_PAIR_WAITING: ESP_LOGI(TAG, "relay pairing pending; retry next wake"); break;
                    case RELAY_PAIR_EXPIRED: ESP_LOGW(TAG, "relay pairing code expired; re-provision"); break;
                    default:                 ESP_LOGW(TAG, "relay pairing step failed; retry next wake"); break;
                }
            }
            // A button gesture now works over relay (server/relay v0.240.0+): it
            // rides the status body. tap -> BTN_TAP_BUTTON ("right"); 5 s hold -> "refresh".
            const char *btn = want_refresh ? "refresh"
                            : (want_next && BTN_TAP_BUTTON[0]) ? BTN_TAP_BUTTON
                            : NULL;
            uint32_t button_ev = btn ? ++s_button_event_seq : 0;
            if (relay_ready()) {
                next = relay_run_loop(btn, button_ev);
                // Post-button window: home learns of the press on its next relay poll
                // (~30 s) then renders a response. Stay awake and keep polling the
                // mailbox until it arrives, so the press feels responsive. Only when
                // nothing was already staged this wake; break on the first NEW frame
                // (painted after wifi_stop, radio off). No new-press chaining.
                if (btn && relay_pending_frame() == NULL && !relay_pairing_revoked()) {
                    ESP_LOGI(TAG, "relay button window: up to %d s awake, polling",
                             RELAY_BUTTON_WINDOW_S);
                    int64_t deadline = esp_timer_get_time() +
                                       (int64_t)RELAY_BUTTON_WINDOW_S * 1000000;
                    while (esp_timer_get_time() < deadline && !relay_pairing_revoked()) {
                        vTaskDelay(pdMS_TO_TICKS(RELAY_BUTTON_POLL_MS));
                        if (relay_poll_frame()) break;   // NEW response staged
                    }
                    ESP_LOGI(TAG, "relay button window closed");
                }
            } else {
                next = config_get_sleep_s(SLEEP_INTERVAL_DEFAULT_S);
            }
        } else if (transport == 0) {
            // MQTT has no server_time, so it is the one path that needs a real
            // clock source (mqtts:// cert validity). Sync only when the clock
            // is implausible — the C3 RTC persists across deep sleep, so this
            // normally fires once per power-on. REST stays NTP-free (0.3.0).
            if (time(NULL) < CLOCK_SANE_EPOCH) wifi_sync_ntp();
            // Button dispatch is REST-only: the server dispatches button actions
            // on its HTTP endpoints, not over MQTT's push-only frame topic.
            if (want_refresh || want_next)
                ESP_LOGW(TAG, "button gesture ignored: not supported on MQTT transport");
            next = mqtt_run_loop(reason);
        } else {
            // https cert validation needs a plausible wall clock too (validity
            // window check) — same sanity pattern as mqtts. Plain http skips it.
            char srv[160] = {0};
            config_get_server_url(srv, sizeof srv);
            if (strncmp(srv, "https://", 8) == 0 && time(NULL) < CLOCK_SANE_EPOCH)
                wifi_sync_ntp();
            // A button gesture this wake gets a fresh, RTC-retained event id.
            // "refresh" (5 s hold) re-renders in place; a tap sends BTN_TAP_BUTTON
            // (deck next) unless it's disabled (empty).
            const char *btn = want_refresh ? "refresh"
                            : (want_next && BTN_TAP_BUTTON[0]) ? BTN_TAP_BUTTON
                            : NULL;
            uint32_t button_ev = btn ? ++s_button_event_seq : 0;
            next = rest_run_loop(reason, btn, button_ev);
        }
    } else {
        ESP_LOGW(TAG, "WiFi failed; keeping last image, retry next wake");
    }
    wifi_stop();

    // A revoked pairing is terminal (two consecutive 401 wakes; server/relay
    // v0.240.0+). Drop it (keeping the relay URL), show setup so a fresh code
    // re-pairs, and sleep. Re-pair is a physical action anyway, so we don't
    // auto-open AP mode here — the setup splash tells the user what to do.
    if (use_relay && relay_pairing_revoked()) {
        ESP_LOGW(TAG, "relay pairing revoked (2 wakes of 401); forgetting + re-pair splash");
        relay_forget_revoked_pairing();
        splash_show_revoked();   // "Unpaired — hold button 20s" (not the AP-join setup splash)
        power_deep_sleep((uint32_t)next);   // no return
    }

    // One-shot after provisioning (any transport): WiFi itself failed with a
    // recognisable misconfiguration signature — reopen the portal with the
    // matching banner while the user is still nearby (docs item 1). Wrong
    // password and no-such-network get distinct messages so the user fixes the
    // right field. Anything else (transient outage) keeps the silent retry
    // loop, so a provisioned device never drops to AP mode on a router blip.
    if (just_provisioned && !wifi_ok) {
        const char *note = NULL;
        if (wifi_fail_looks_like_bad_password()) {
            note = "Couldn&rsquo;t join the WiFi network &mdash; the password "
                   "looks wrong. Please re-enter it.";
        } else if (wifi_fail_looks_like_no_ap()) {
            note = "Couldn&rsquo;t find the WiFi network &mdash; check the "
                   "network name. If the network has no password, leave the "
                   "password field blank.";
        }
        if (note) {
            ESP_LOGW(TAG, "just provisioned and WiFi failed with a config signature; reopening portal");
            splash_show_setup();
            if (provisioning_run_blocking(note) == ESP_OK) {
                esp_restart();                          // saved -> retry with new settings
            }
            power_deep_sleep(SLEEP_INTERVAL_DEFAULT_S); // timeout: normal cycle next wake
        }
    }

    // Deliberately NO banner when WiFi is fine but the server/broker is
    // unreachable (removed 2026-07-19, docs item 14): the backend being down at
    // the exact first boot is usually the user's own doing (restarting the
    // Tesserae docker container mid-setup) — bouncing a correctly configured
    // frame back into the portal for that is a false alarm. The frame keeps
    // retrying on its own (30 s discover cadence while unpaired, heartbeat
    // every wake once paired) and catches up when the backend returns; a
    // genuinely wrong URL is recovered via the 20 s button-hold portal.

    // Transport-agnostic contract: all network I/O finishes (incl. a graceful
    // MQTT stop — no LWT), then radio off, then paint. Radio + EPD refresh
    // together is the worst-case rail load on a board that browns out on radio
    // spikes, and the radio idling through a 13-22 s refresh burns ~80 mA for
    // nothing. EPD init is lazy for the same reason: most wakes end unchanged
    // and the panel never powers up at all.
    const uint8_t *fb = use_relay        ? relay_pending_frame()
                      : (transport == 0) ? mqtt_pending_frame()
                                         : rest_pending_frame();
    if (fb) {
        ESP_LOGI(TAG, "painting new frame (radio off)");
        esp_err_t err = epd_present(fb);
        if (err == ESP_OK) {
            // Commit only after refresh and panel shutdown have both succeeded.
            if (use_relay)           relay_frame_painted();
            else if (transport == 0) mqtt_frame_painted();
            else                     rest_frame_painted();
        } else {
            ESP_LOGW(TAG, "display failed (%s); frame reference unchanged, retry next wake",
                     esp_err_to_name(err));
        }
    }
    power_deep_sleep((uint32_t)next);   // no return
}
