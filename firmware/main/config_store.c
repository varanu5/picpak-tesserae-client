// config_store.c — NVS-backed config with secrets.h fallbacks.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "config_store.h"
#include "defaults.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "cfg";

#define NS_WIFI  "wifi"
#define NS_REST  "rest"
#define NS_STATE "state"
#define NS_RELAY "relay"

esp_err_t config_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (%s); reinitialising", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

// Read a string key; returns length copied (0 if missing/empty). Always NUL-terminates.
static size_t nvs_get_str_or_empty(const char *ns, const char *key, char *out, size_t out_sz) {
    if (out_sz == 0) return 0;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = out_sz;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) { out[0] = '\0'; return 0; }
    return strnlen(out, out_sz);
}

static void nvs_set_str_commit(const char *ns, const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

bool config_get_wifi(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    if (!ssid_sz || !pass_sz) return false;
    ssid[0] = pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READONLY, &h) == ESP_OK) {
        uint8_t cleared = 0;
        nvs_get_u8(h, "cleared", &cleared);
        if (cleared) { nvs_close(h); return false; }
        size_t ns = ssid_sz, np = pass_sz;
        bool saved = nvs_get_str(h, "ssid", ssid, &ns) == ESP_OK && ssid[0];
        // An explicitly saved empty password is an open network, not a request
        // to reuse the compile-time password from a different network.
        bool saved_password = nvs_get_str(h, "pass", pass, &np) == ESP_OK;
        nvs_close(h);
        if (saved) {
            if (!saved_password) strlcpy(pass, WIFI_DEFAULT_PASS, pass_sz);
            return true;
        }
    }
    strlcpy(ssid, WIFI_DEFAULT_SSID, ssid_sz);
    strlcpy(pass, WIFI_DEFAULT_PASS, pass_sz);
    return ssid[0] != '\0';
}

// --- fast-connect AP hint (BSSID + channel), stored in the wifi namespace ---
bool config_get_ap_hint(uint8_t bssid[6], uint8_t *chan) {
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = 6; uint8_t c = 0;
    bool ok = (nvs_get_blob(h, "ap_bssid", bssid, &l) == ESP_OK && l == 6 &&
               nvs_get_u8(h, "ap_chan", &c) == ESP_OK && c >= 1 && c <= 14);
    nvs_close(h);
    if (ok && chan) *chan = c;
    return ok;
}

void config_set_ap_hint(const uint8_t bssid[6], uint8_t chan) {
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READWRITE, &h) != ESP_OK) return;
    // Skip the write when unchanged, to spare flash wear across wakes.
    uint8_t cur_b[6]; size_t l = 6; uint8_t cur_c = 0;
    bool same = (nvs_get_blob(h, "ap_bssid", cur_b, &l) == ESP_OK && l == 6 &&
                 memcmp(cur_b, bssid, 6) == 0 &&
                 nvs_get_u8(h, "ap_chan", &cur_c) == ESP_OK && cur_c == chan);
    if (!same) {
        nvs_set_blob(h, "ap_bssid", bssid, 6);
        nvs_set_u8(h, "ap_chan", chan);
        nvs_commit(h);
    }
    nvs_close(h);
}

void config_clear_ap_hint(void) {
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ap_bssid");
    nvs_erase_key(h, "ap_chan");
    nvs_commit(h);
    nvs_close(h);
}

void config_get_server_url(char *out, size_t out_sz) {
    if (nvs_get_str_or_empty(NS_REST, "server_url", out, out_sz) == 0)
        strlcpy(out, REST_DEFAULT_SERVER_URL, out_sz);
}

void config_get_device_token(char *out, size_t out_sz) {
    nvs_get_str_or_empty(NS_REST, "device_token", out, out_sz);
}
void config_set_device_token(const char *token) {
    nvs_set_str_commit(NS_REST, "device_token", token);
}

void config_get_device_id(char *out, size_t out_sz) {
    nvs_get_str_or_empty(NS_REST, "device_id", out, out_sz);
}
void config_set_device_id(const char *id) {
    nvs_set_str_commit(NS_REST, "device_id", id);
}

void config_get_etag(char *out, size_t out_sz) {
    nvs_get_str_or_empty(NS_REST, "frame_etag", out, out_sz);
}
void config_set_etag(const char *etag) {
    nvs_set_str_commit(NS_REST, "frame_etag", etag);
}

void config_set_wifi(const char *ssid, const char *pass) {
    char old_ssid[33], old_pass[65];
    bool same = config_get_wifi(old_ssid, sizeof old_ssid, old_pass, sizeof old_pass)
                && ssid && strcmp(ssid, old_ssid) == 0;
    if (ssid && ssid[0]) nvs_set_str_commit(NS_WIFI, "ssid", ssid);
    if (pass && pass[0]) nvs_set_str_commit(NS_WIFI, "pass", pass);
    else if (!same) nvs_set_str_commit(NS_WIFI, "pass", "");
    if (ssid && ssid[0]) {
        nvs_handle_t h;
        if (nvs_open(NS_WIFI, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "cleared", 0);
            nvs_set_u8(h, "ble_recovery", 0);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}
void config_set_server_url(const char *url) { nvs_set_str_commit(NS_REST, "server_url", url); }
void config_set_pairing_code(const char *code) { nvs_set_str_commit(NS_REST, "pair_code", code ? code : ""); }
void config_get_pairing_code(char *out, size_t out_sz) { nvs_get_str_or_empty(NS_REST, "pair_code", out, out_sz); }
void config_set_mqtt(const char *uri, const char *user, const char *pass) {
    if (uri)  nvs_set_str_commit(NS_REST, "mqtt_uri",  uri);
    if (user) nvs_set_str_commit(NS_REST, "mqtt_user", user);
    if (pass && pass[0]) nvs_set_str_commit(NS_REST, "mqtt_pass", pass);
}
void config_get_mqtt(char *uri, size_t uri_sz, char *user, size_t user_sz,
                     char *pass, size_t pass_sz) {
    if (nvs_get_str_or_empty(NS_REST, "mqtt_uri", uri, uri_sz) == 0)
        strlcpy(uri, MQTT_DEFAULT_URI, uri_sz);
    if (nvs_get_str_or_empty(NS_REST, "mqtt_user", user, user_sz) == 0)
        strlcpy(user, MQTT_DEFAULT_USER, user_sz);
    if (nvs_get_str_or_empty(NS_REST, "mqtt_pass", pass, pass_sz) == 0)
        strlcpy(pass, MQTT_DEFAULT_PASS, pass_sz);
}
void config_get_frame_url(char *out, size_t out_sz) {
    nvs_get_str_or_empty(NS_REST, "frame_url", out, out_sz);
}
void config_set_frame_url(const char *url) {
    nvs_set_str_commit(NS_REST, "frame_url", url ? url : "");
}
void config_clear_frame_ref(void) {
    // Clear all three transports' dedup keys unconditionally rather than branching
    // on the active transport: three NVS writes on a path that fires at most once
    // per splash, and immune to the very bug this fixes (a fourth transport gets
    // it for free once its key is added here). nvs_set_str short-circuits an
    // identical value, so clearing an already-empty key is not a write.
    config_set_etag("");            // REST  (rest/frame_etag)
    config_set_frame_url("");       // MQTT  (rest/frame_url)
    config_set_relay_etag("");      // relay (relay/etag)
}
void config_set_transport(uint8_t mode) {
    nvs_handle_t h;
    if (nvs_open(NS_REST, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "transport", mode); nvs_commit(h); nvs_close(h);
}
uint8_t config_get_transport(uint8_t fallback) {
    nvs_handle_t h; uint8_t v = fallback;
    if (nvs_open(NS_REST, NVS_READONLY, &h) != ESP_OK) return fallback;
    if (nvs_get_u8(h, "transport", &v) != ESP_OK) v = fallback;
    nvs_close(h);
    return v;
}
// Refresh waveform (0=5s, 1=10s, 2=native). Transport-agnostic device setting,
// so it lives in the state namespace next to sleep_s. Portal-set, epd_init reads it.
void config_set_waveform(uint8_t mode) {
    (void)config_save_waveform(mode);
}
esp_err_t config_save_waveform(uint8_t mode) {
    if (mode > 2) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_STATE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint8_t current;
    if (nvs_get_u8(h, "waveform", &current) == ESP_OK && current == mode) {
        nvs_close(h);
        return ESP_OK;
    }
    err = nvs_set_u8(h, "waveform", mode);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
uint8_t config_get_waveform(uint8_t fallback) {
    nvs_handle_t h; uint8_t v = fallback;
    if (nvs_open(NS_STATE, NVS_READONLY, &h) != ESP_OK) return fallback;
    if (nvs_get_u8(h, "waveform", &v) != ESP_OK) v = fallback;
    nvs_close(h);
    return v <= 2 ? v : fallback;
}
static bool read_photo_config(uint8_t value[33]) {
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 33;
    esp_err_t err = nvs_get_blob(h, "photo_config", value, &len);
    nvs_close(h);
    return err == ESP_OK && len == 33 && value[0] == 1;
}
bool config_screen_is_bluetooth(void) {
    uint8_t value[33] = {0};
    bool result = read_photo_config(value);
    memset(value, 0, sizeof value);
    return result;
}
bool config_get_photo_key(uint8_t key[32]) {
    uint8_t value[33] = {0};
    bool result = read_photo_config(value);
    if (result) memcpy(key, value + 1, 32);
    memset(value, 0, sizeof value);
    return result;
}
esp_err_t config_save_screen_mode(bool bluetooth, const uint8_t key[32]) {
    if (bluetooth && !key) return ESP_ERR_INVALID_ARG;
    uint8_t value[33] = {0};
    value[0] = bluetooth ? 1 : 0;
    if (bluetooth) memcpy(value + 1, key, 32);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_STATE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "photo_config", value, sizeof value);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    memset(value, 0, sizeof value);
    return err;
}

void config_set_paired_pending(bool pending) {
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "paired_pen", pending ? 1 : 0); nvs_commit(h); nvs_close(h);
}

esp_err_t config_save_wifi(const char *ssid, const char *password) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !password || strlen(password) > 64)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", password);
    if (err == ESP_OK) err = nvs_set_u8(h, "cleared", 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "ble_recovery", 0);
    if (err == ESP_OK) {
        nvs_erase_key(h, "ap_bssid");
        nvs_erase_key(h, "ap_chan");
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_clear_wifi(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_set_u8(h, "cleared", 1);
    if (err == ESP_OK) err = nvs_set_u8(h, "ble_recovery", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool config_take_ble_recovery(void) {
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t value = 0;
    nvs_get_u8(h, "ble_recovery", &value);
    if (value) {
        nvs_set_u8(h, "ble_recovery", 0);
        nvs_commit(h);
    }
    nvs_close(h);
    return value != 0;
}

esp_err_t config_factory_reset(void) {
    const char *names[] = {NS_REST, NS_STATE, NS_RELAY};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(names[i], NVS_READWRITE, &h);
        if (err != ESP_OK) return err;
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = config_clear_wifi();
    if (err == ESP_OK) (void)config_take_ble_recovery(); // reset returns to AP setup
    return err;
}
bool config_take_paired_pending(void) {
    nvs_handle_t h; uint8_t v = 0;
    if (nvs_open(NS_STATE, NVS_READWRITE, &h) != ESP_OK) return false;
    if (nvs_get_u8(h, "paired_pen", &v) != ESP_OK) v = 0;
    if (v) { nvs_set_u8(h, "paired_pen", 0); nvs_commit(h); }
    nvs_close(h);
    return v != 0;
}

uint32_t config_get_sleep_s(uint32_t fallback) {
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READONLY, &h) != ESP_OK) return fallback;
    uint32_t v = fallback;
    if (nvs_get_u32(h, "sleep_s", &v) != ESP_OK) v = fallback;
    nvs_close(h);
    return v;
}
void config_set_sleep_s(uint32_t seconds) {
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "sleep_s", seconds);
    nvs_commit(h);
    nvs_close(h);
}

// --- cloud relay ---------------------------------------------------------
// 32-byte blob helpers (pairing private scalar + derived frame key).
static bool nvs_get_blob32(const char *ns, const char *key, uint8_t out[32]) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = 32;
    bool ok = (nvs_get_blob(h, key, out, &l) == ESP_OK && l == 32);
    nvs_close(h);
    return ok;
}
static esp_err_t nvs_set_blob32_commit(const char *ns, const char *key, const uint8_t in[32]) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, in, 32);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool config_relay_ready(void) {
    uint8_t k[32];
    return config_get_relay_key(k);
}
bool config_relay_configured(void) {
    // Buffers must hold the full stored value: nvs_get_str returns
    // ESP_ERR_NVS_INVALID_LENGTH (-> 0 here) if the string is longer than the
    // buffer, which would falsely read a long URL as "unset".
    char url[160];
    if (nvs_get_str_or_empty(NS_RELAY, "url", url, sizeof url) == 0) return false;
    if (config_relay_ready()) return true;
    char code[40];
    return nvs_get_str_or_empty(NS_RELAY, "code", code, sizeof code) > 0;
}
void config_get_relay_url(char *o, size_t n)   { nvs_get_str_or_empty(NS_RELAY, "url", o, n); }
void config_set_relay_url(const char *v)       { nvs_set_str_commit(NS_RELAY, "url", v ? v : ""); }
void config_get_relay_code(char *o, size_t n)  { nvs_get_str_or_empty(NS_RELAY, "code", o, n); }
void config_set_relay_code(const char *v)      { nvs_set_str_commit(NS_RELAY, "code", v ? v : ""); }
bool config_get_relay_priv(uint8_t p[32])      { return nvs_get_blob32(NS_RELAY, "priv", p); }
esp_err_t config_set_relay_priv(const uint8_t p[32]) { return nvs_set_blob32_commit(NS_RELAY, "priv", p); }
void config_get_relay_install(char *o, size_t n){ nvs_get_str_or_empty(NS_RELAY, "install", o, n); }
void config_get_relay_device(char *o, size_t n) { nvs_get_str_or_empty(NS_RELAY, "device", o, n); }
void config_get_relay_token(char *o, size_t n)  { nvs_get_str_or_empty(NS_RELAY, "token", o, n); }
bool config_get_relay_key(uint8_t k[32])       { return nvs_get_blob32(NS_RELAY, "key", k); }
void config_get_relay_etag(char *o, size_t n)  { nvs_get_str_or_empty(NS_RELAY, "etag", o, n); }
void config_set_relay_etag(const char *v)      { nvs_set_str_commit(NS_RELAY, "etag", v ? v : ""); }
void config_get_relay_config_etag(char *o, size_t n){ nvs_get_str_or_empty(NS_RELAY, "cfg_etag", o, n); }
void config_set_relay_config_etag(const char *v)    { nvs_set_str_commit(NS_RELAY, "cfg_etag", v ? v : ""); }

void config_set_relay_paired(const char *install, const char *device,
                             const char *token, const uint8_t key[32]) {
    nvs_handle_t h;
    if (nvs_open(NS_RELAY, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "install", install ? install : "");
    nvs_set_str(h, "device",  device ? device : "");
    nvs_set_str(h, "token",   token ? token : "");
    nvs_set_blob(h, "key", key, 32);
    nvs_erase_key(h, "priv");   // scalar no longer needed once the key exists
    nvs_commit(h);
    nvs_close(h);
}
void config_clear_relay(void) {
    nvs_handle_t h;
    if (nvs_open(NS_RELAY, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}
void config_forget_relay_pairing(void) {
    nvs_handle_t h;
    if (nvs_open(NS_RELAY, NVS_READWRITE, &h) != ESP_OK) return;
    static const char *const keys[] = {
        "code", "priv", "install", "device", "token", "key", "etag", "cfg_etag",
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        nvs_erase_key(h, keys[i]);   // ignore ESP_ERR_NVS_NOT_FOUND
    nvs_commit(h);
    nvs_close(h);
    // "url" deliberately kept.
}

bool config_lowbatt_screen_pending(void) {
    nvs_handle_t h;
    uint8_t pending = 0;
    if (nvs_open(NS_STATE, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, "batt_screen", &pending);
    nvs_close(h);
    return pending != 0;
}

esp_err_t config_set_lowbatt_screen(bool pending) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_STATE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        uint8_t current = 0;
        esp_err_t read = nvs_get_u8(h, "batt_screen", &current);
        if ((read == ESP_OK && current == (uint8_t)pending) ||
            (read == ESP_ERR_NVS_NOT_FOUND && !pending)) {
            nvs_close(h);
            return ESP_OK;
        }
        err = nvs_set_u8(h, "batt_screen", (uint8_t)pending);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "battery screen state not saved: %s", esp_err_to_name(err));
    return err;
}
