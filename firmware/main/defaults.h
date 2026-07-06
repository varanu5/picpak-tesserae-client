// defaults.h — compile-time config defaults, overridable via git-ignored secrets.h.
// SPDX-License-Identifier: AGPL-3.0-or-later
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

#ifndef FW_VERSION
#define FW_VERSION          "0.2.0-dev"
#endif
#define DEVICE_KIND         "picpak_client"

// Deep-sleep bounds + fallback (seconds).
#define SLEEP_INTERVAL_DEFAULT_S   900
// While waiting for the admin to claim this device in the Tesserae UI, retry
// discover on this cadence (server also suggests one via retry_after_s).
#define REST_DISCOVER_RETRY_S      30
// Pairing rejected (bad/expired code -> 403) or rate-limited (429): back off
// hard so a wrong code doesn't hammer /register every 30s. One hour matches the
// reference firmware's deep-sleep-on-403 behaviour.
#define REST_PAIR_REJECT_RETRY_S   3600
#define SLEEP_INTERVAL_MIN_S       30
#define SLEEP_INTERVAL_MAX_S       (7 * 24 * 60 * 60)

// DHCP hostname advertised to the router when we join as a STA (client).
// ESP-IDF defaults this to "espressif"; override so the frame is identifiable
// in the router's client list.
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

// --- M4 captive-portal provisioning ---
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
