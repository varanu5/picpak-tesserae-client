// defaults.h — compile-time config defaults, overridable via git-ignored secrets.h.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

#ifndef WIFI_DEFAULT_SSID
#define WIFI_DEFAULT_SSID   ""
#endif
#ifndef WIFI_DEFAULT_PASS
#define WIFI_DEFAULT_PASS   ""
#endif
#ifndef REST_DEFAULT_SERVER_URL
#define REST_DEFAULT_SERVER_URL ""
#endif
#ifndef MQTT_DEFAULT_URI
#define MQTT_DEFAULT_URI    ""
#endif
#ifndef MQTT_DEFAULT_USER
#define MQTT_DEFAULT_USER   ""
#endif
#ifndef MQTT_DEFAULT_PASS
#define MQTT_DEFAULT_PASS   ""
#endif

#ifndef FW_VERSION
#define FW_VERSION          "0.5.0"
#endif
#define DEVICE_KIND         "picpak_client"

// Deep-sleep bounds + fallback (seconds).
#define SLEEP_INTERVAL_DEFAULT_S   900
// Fallback discover-retry cadence, used ONLY when the server can't be reached
// (not running yet, wrong URL, transient outage) — a freshly set-up device
// shouldn't wake every 30 s hammering a server that isn't up. When the server
// IS up but hasn't claimed the device, it dictates the (fast) cadence via
// retry_after_s, which overrides this — so real onboarding stays responsive
// while the unreachable-server case backs off. Matches the normal sleep
// interval; press the front button to onboard immediately once the server is up.
#define REST_DISCOVER_RETRY_S      900
// Pairing rejected (bad/expired code -> 403) or rate-limited (429): back off
// hard so a wrong code doesn't hammer /register every 30s. One hour matches the
// reference firmware's deep-sleep-on-403 behaviour.
#define REST_PAIR_REJECT_RETRY_S   3600
#define SLEEP_INTERVAL_MIN_S       30
#define SLEEP_INTERVAL_MAX_S       (7 * 24 * 60 * 60)

// DHCP hostname fallback prefix. The hostname is normally the provisioned
// device id (so the router's client list matches Tesserae's device list); an
// unnamed frame advertises WIFI_HOSTNAME-<last 3 MAC bytes> so multiple
// PicPaks stay distinguishable. ESP-IDF's own default would be "espressif".
#define WIFI_HOSTNAME              "tesserae-picpak"

// WiFi connect tuning.
#define WIFI_CONNECT_RETRIES       5
#define WIFI_CONNECT_TIMEOUT_MS    15000
// PicPak's power delivery is marginal: at default max TX power (~20 dBm) the
// radio's current spike browns the board out the instant it transmits, so cap
// it. Units = 0.25 dBm; 52 = 13 dBm.
#define WIFI_TX_POWER_QDBM         40   /* 10 dBm; browns out at 13 dBm on low battery */

// Boot flashing-safety window: hold the button at boot to stay awake this long
// (USB enumerated) before the normal sleep cycle, so a sleeping build is always
// re-flashable.
#define BOOT_HOLD_WINDOW_MS        15000

// --- Captive-portal provisioning ---
#define PROVISION_AP_SSID          "Tesserae-Setup"
#define PROVISION_AP_PASS          "tesserae"        // >= 8 chars
#define PROVISION_HOLD_MS          20000             // hold button this long -> re-provision
#define PROVISION_PORTAL_TIMEOUT_S 600               // portal idle timeout -> deep sleep
#define PROVISION_SCAN_MAX         12

// On a brownout-triggered wake, defer WiFi/paint and deep-sleep this long so the
// battery can recover, instead of rapid reset-looping (which drains it faster).
#define BROWNOUT_RECOVERY_SLEEP_S  180
// Let the power rail settle after boot before the WiFi radio's current spike.
#define WIFI_SETTLE_MS             250

// --- MQTT transport (Mode 0) ---
// Cap on waiting for the retained frame/bin message after subscribing.
#define MQTT_WAIT_RETAINED_MS      8000
// Settle window after the frame message so a retained config payload queued
// right behind it isn't dropped by teardown (reference-proven quirk).
#define MQTT_SETTLE_MS             500
// Heartbeat publish: wait this long for the broker PUBACK (QoS 1).
#define MQTT_PUBACK_WAIT_MS        5000
#define MQTT_KEEPALIVE_S           30
// Clock sanity floor (epoch seconds, 2025-06-16). MQTT has no server_time; if
// time(NULL) is below this at an MQTT wake the clock was never set (or was
// lost to a power-off), so run the best-effort SNTP sync — needed for
// mqtts:// cert validation, harmless otherwise. The C3 RTC persists across
// deep sleep, so this normally fires once per power-on.
#define CLOCK_SANE_EPOCH           1750000000
