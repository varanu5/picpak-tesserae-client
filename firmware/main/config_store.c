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
    if (nvs_get_str_or_empty(NS_WIFI, "ssid", ssid, ssid_sz) == 0)
        strlcpy(ssid, WIFI_DEFAULT_SSID, ssid_sz);
    if (nvs_get_str_or_empty(NS_WIFI, "pass", pass, pass_sz) == 0)
        strlcpy(pass, WIFI_DEFAULT_PASS, pass_sz);
    return ssid[0] != '\0';
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
    if (ssid && ssid[0]) nvs_set_str_commit(NS_WIFI, "ssid", ssid);
    if (pass && pass[0]) nvs_set_str_commit(NS_WIFI, "pass", pass);  // blank => keep existing
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
void config_set_paired_pending(bool pending) {
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "paired_pen", pending ? 1 : 0); nvs_commit(h); nvs_close(h);
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
