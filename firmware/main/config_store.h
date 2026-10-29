// config_store.h — Shared device settings and independent saved connections.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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

// Display source is independent of the selected network connection.
// A single committed blob stores mode + the 32-byte photo authorization key.
bool config_screen_is_bluetooth(void);
bool config_get_photo_key(uint8_t key[32]);
esp_err_t config_save_screen_mode(bool bluetooth, const uint8_t key[32]);

// --- portal write path ---
void config_set_wifi(const char *ssid, const char *pass);   // blank pass keeps the password only for the same SSID
void config_set_server_url(const char *url);
void config_set_transport(uint8_t mode);                    // 0=MQTT, 1=Local REST, 2=Relay, 3=Cloud
uint8_t config_get_transport(uint8_t fallback);
void config_set_waveform(uint8_t mode);                     // 0=5s, 1=10s, 2=native MTP
uint8_t config_get_waveform(uint8_t fallback);
// Checked BLE writes: acknowledge only after NVS has committed successfully.
esp_err_t config_save_waveform(uint8_t mode);
esp_err_t config_save_wifi(const char *ssid, const char *password);
esp_err_t config_clear_wifi(void); // preserves REST/MQTT/relay and speed; opens BLE once
esp_err_t config_factory_reset(void);
bool config_take_ble_recovery(void);
void config_set_pairing_code(const char *code);
void config_get_pairing_code(char *out, size_t out_sz);   // empty if none set

// Relay credentials remain saved while another connection is active.
bool config_relay_ready(void);        // selected, valid mailbox credentials
bool config_relay_configured(void);   // Relay selected, including after removal
void config_get_relay_url(char *out, size_t out_sz);
void config_set_relay_url(const char *url);
void config_get_relay_code(char *out, size_t out_sz);
void config_set_relay_code(const char *code);
bool config_get_relay_priv(uint8_t priv[32]);      // false if absent
esp_err_t config_set_relay_priv(const uint8_t priv[32]);
esp_err_t config_set_relay_paired(const char *install, const char *device,
                             const char *token, const uint8_t key[32]);
void config_get_relay_install(char *out, size_t out_sz);
void config_get_relay_device(char *out, size_t out_sz);
void config_get_relay_token(char *out, size_t out_sz);
bool config_get_relay_key(uint8_t key[32]);        // false if absent
void config_get_relay_etag(char *out, size_t out_sz);
void config_set_relay_etag(const char *etag);
void config_get_relay_config_etag(char *out, size_t out_sz);
void config_set_relay_config_etag(const char *etag);
void config_clear_relay(void);        // clear credentials, retain endpoint
// Mark removal, keeping Relay selected and its endpoint/identity visible.
void config_forget_relay_pairing(void);
void config_set_mqtt(const char *uri, const char *user, const char *pass);  // blank password keeps it only for the same broker/user
// MQTT broker config (NVS -> secrets -> ""). All outputs always NUL-terminated.
void config_get_mqtt(char *uri, size_t uri_sz, char *user, size_t user_sz,
                     char *pass, size_t pass_sz);
// Last successfully painted frame URL (MQTT skip-check; separate from the REST
// ETag so a transport switch causes one harmless refetch, never a false skip).
void config_get_frame_url(char *out, size_t out_sz);
void config_set_frame_url(const char *url);
// Forget "the panel is already showing the current frame" for EVERY transport
// (REST ETag, MQTT frame URL, relay ETag). Call whenever firmware paints something
// outside the normal frame path (a splash), so the next poll returns a full
// 200 / new-URL and repaints the live photo rather than 304-ing onto the splash.
void config_clear_frame_ref(void);
void config_set_paired_pending(bool pending);
bool config_take_paired_pending(void);   // returns flag, then clears it (one-shot)

// Remember a charge screen until another image has been painted successfully.
bool config_lowbatt_screen_pending(void);
esp_err_t config_set_lowbatt_screen(bool pending);

#define SETUP_CLOUD_URL "https://cloud.tesserae.ink"
typedef enum { CONFIG_PROFILE_MQTT=0, CONFIG_PROFILE_LOCAL=1,
               CONFIG_PROFILE_RELAY=2, CONFIG_PROFILE_CLOUD=3 } config_profile_t;
typedef struct {
    char server[160], id[64], user[64];
    bool paired, pending;
} config_profile_view_t;
esp_err_t config_profiles_init(void);
config_profile_t config_active_profile(void);
void config_get_profile(config_profile_t mode, config_profile_view_t *out);
bool config_rest_unpaired(void);
esp_err_t config_note_rest_unregistered(void);
bool config_relay_revoked(void);
esp_err_t config_save_pairing(const char *id, const char *token, bool consume_code);
// Save only the selected connection; blank codes preserve matching credentials.
esp_err_t config_save_connection(config_profile_t mode, const char *server,
    const char *id, const char *code, const char *user, const char *password);
