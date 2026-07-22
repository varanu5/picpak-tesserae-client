// config_store.h — NVS-backed config with secrets.h fallbacks.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t config_init(void);   // nvs_flash_init (+ erase-recover on corruption)

// WiFi credentials. Returns true if a non-empty SSID is available (NVS or secrets).
bool config_get_wifi(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);

// Tesserae server base URL (NVS -> secrets -> "").
void config_get_server_url(char *out, size_t out_sz);

// Bearer device token (empty if unpaired).
void config_get_device_token(char *out, size_t out_sz);
void config_set_device_token(const char *token);

// Server-assigned device id (used in /frame and /status URLs).
void config_get_device_id(char *out, size_t out_sz);
void config_set_device_id(const char *id);

// Frame ETag for If-None-Match / 304.
void config_get_etag(char *out, size_t out_sz);
void config_set_etag(const char *etag);

// Server-configured sleep interval (seconds).
uint32_t config_get_sleep_s(uint32_t fallback);
void     config_set_sleep_s(uint32_t seconds);

// Fast-connect AP hint: the last associated AP's BSSID (6 bytes) + primary
// channel, cached in the "wifi" namespace to skip the scan on the next wake.
// get returns false when unset/invalid; set skips the write when unchanged (flash
// wear); clear drops it (stale hint -> full-scan fallback re-caches the real AP).
bool config_get_ap_hint(uint8_t bssid[6], uint8_t *chan);
void config_set_ap_hint(const uint8_t bssid[6], uint8_t chan);
void config_clear_ap_hint(void);

// --- portal write path ---
void config_set_wifi(const char *ssid, const char *pass);   // blank/NULL pass keeps existing
void config_set_server_url(const char *url);
void config_set_transport(uint8_t mode);                    // 0=MQTT, 1=REST
uint8_t config_get_transport(uint8_t fallback);
void config_set_pairing_code(const char *code);
void config_get_pairing_code(char *out, size_t out_sz);   // empty if none set
void config_set_mqtt(const char *uri, const char *user, const char *pass);  // blank/NULL pass keeps existing
// MQTT broker config (NVS -> secrets -> ""). All outputs always NUL-terminated.
void config_get_mqtt(char *uri, size_t uri_sz, char *user, size_t user_sz,
                     char *pass, size_t pass_sz);
// Last successfully painted frame URL (MQTT skip-check; separate from the REST
// ETag so a transport switch causes one harmless refetch, never a false skip).
void config_get_frame_url(char *out, size_t out_sz);
void config_set_frame_url(const char *url);
void config_set_paired_pending(bool pending);
bool config_take_paired_pending(void);   // returns flag, then clears it (one-shot)
