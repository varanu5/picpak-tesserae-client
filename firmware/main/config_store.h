// config_store.h — NVS-backed config with secrets.h fallbacks.
// SPDX-License-Identifier: AGPL-3.0-or-later
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

// --- portal write path (M4) ---
void config_set_wifi(const char *ssid, const char *pass);   // blank/NULL pass keeps existing
void config_set_server_url(const char *url);
void config_set_transport(uint8_t mode);                    // 0=MQTT, 1=REST
uint8_t config_get_transport(uint8_t fallback);
void config_set_pairing_code(const char *code);
void config_get_pairing_code(char *out, size_t out_sz);   // empty if none set
void config_set_mqtt(const char *uri, const char *user, const char *pass);  // inert until M5
void config_set_paired_pending(bool pending);
bool config_take_paired_pending(void);   // returns flag, then clears it (one-shot)
