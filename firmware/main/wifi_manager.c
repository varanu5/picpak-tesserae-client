// wifi_manager.c — STA connect + NTP.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "wifi_manager.h"
#include "config_store.h"
#include "defaults.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_eg;
#define BIT_CONN BIT0
#define BIT_FAIL BIT1
static int s_retries;
static esp_netif_t *s_netif;
static uint8_t s_last_disc_reason;   // last WIFI_EVENT_STA_DISCONNECTED reason
// Reason latched at the moment wifi_start_sta() gives up. The classifier must
// read this, not s_last_disc_reason: wifi_stop()'s teardown fires one more
// disconnect event (reason ASSOC_LEAVE) that clobbers the live value before
// main.c gets to ask (hardware-observed: reason=2 became reason=8).
static uint8_t s_fail_reason;
// Only auto-connect on STA_START while wifi_start_sta() is driving the radio.
// The banner-path portal re-enters provisioning after a failed connect, and
// its pre-AP scan brings the STA up just to look around — without this gate
// the handler fired esp_wifi_connect() at the stored (bad) creds during that
// scan and the portal's network picker came up empty (hardware-observed).
static bool s_autoconnect;

// DHCP hostname: the provisioned device id (the name the frame already has in
// Tesserae, e.g. "picpak-red") so the router's client list matches the server's
// device list; falls back to WIFI_HOSTNAME + a MAC suffix so multiple frames
// stay distinguishable before they're named. '_' is valid in a device id but
// not in a hostname (RFC 952/1123), so it maps to '-'.
static void build_hostname(char *out, size_t out_sz) {
    char dev_id[33] = {0};
    config_get_device_id(dev_id, sizeof dev_id);
    if (dev_id[0]) {
        size_t o = 0;
        for (size_t i = 0; dev_id[i] && o < out_sz - 1; i++)
            out[o++] = (dev_id[i] == '_') ? '-' : dev_id[i];
        out[o] = '\0';
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_sz, WIFI_HOSTNAME "-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_autoconnect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_last_disc_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        if (!s_autoconnect) { /* not our attempt (e.g. portal scan teardown) */ }
        else if (s_retries < WIFI_CONNECT_RETRIES) { s_retries++; esp_wifi_connect(); }
        else xEventGroupSetBits(s_eg, BIT_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_eg, BIT_CONN);
    }
}

esp_err_t wifi_start_sta(void) {
    char ssid[33] = {0}, pass[65] = {0};
    if (!config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGE(TAG, "no WiFi credentials (set secrets.h or provision)");
        return ESP_FAIL;
    }
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    // Set the DHCP hostname before the interface comes up, so the router shows
    // the frame's name instead of the ESP-IDF default "espressif".
    char hostname[33];
    build_hostname(hostname, sizeof hostname);
    esp_err_t herr = esp_netif_set_hostname(s_netif, hostname);
    if (herr != ESP_OK) ESP_LOGW(TAG, "set_hostname: %s", esp_err_to_name(herr));
    else ESP_LOGI(TAG, "hostname: %s", hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    // Refuse to join an unencrypted AP when a password is stored — without the
    // threshold (default OPEN) a rogue open AP broadcasting our SSID would get
    // the bearer token sent to it in cleartext.
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_retries = 0;
    s_autoconnect = true;   // STA_START -> connect, for this attempt only
    ESP_ERROR_CHECK(esp_wifi_start());
    // PicPak brownout guard: cap TX power before the radio transmits.
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);

    ESP_LOGI(TAG, "connecting to '%s'", ssid);
    EventBits_t b = xEventGroupWaitBits(s_eg, BIT_CONN | BIT_FAIL, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (b & BIT_CONN) { ESP_LOGI(TAG, "connected"); s_fail_reason = 0; return ESP_OK; }
    s_fail_reason = s_last_disc_reason;   // latch before wifi_stop() can clobber it
    ESP_LOGW(TAG, "connect failed/timeout (last disconnect reason=%d)", s_fail_reason);
    return ESP_FAIL;
}

bool wifi_fail_looks_like_bad_password(void) {
    // The designed signature set (docs/new_features_to_add.md item 1): a wrong
    // WPA2 password surfaces as an auth failure or a handshake timeout, while a
    // typo'd/absent SSID gives NO_AP_FOUND and a transient outage gives
    // BEACON_TIMEOUT/ASSOC_LEAVE — those must NOT reopen the portal.
    // AUTH_EXPIRE added 2026-07-18: hardware-observed as the wrong-password
    // reason on the user's AP (reason=2 on every retry). It can also occur on
    // a transiently overloaded AP, but this classifier is only consulted on
    // the one-shot first boot after provisioning, so the blast radius is one
    // extra portal offer right after a save — acceptable.
    switch (s_fail_reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return true;
        default:
            return false;
    }
}

bool wifi_fail_looks_like_no_ap(void) {
    // "No joinable AP by that name": a typo'd SSID (201), or a name that exists
    // but with incompatible security (210/211 — e.g. a password was provisioned
    // for an open network, which our WPA2 threshold then filters out). Like the
    // bad-password classifier, only consulted on the one-shot first boot after
    // provisioning — a router that's merely off during a normal wake stays on
    // the silent retry path.
    switch (s_fail_reason) {
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return true;
        default:
            return false;
    }
}

void wifi_stop(void) {
    s_autoconnect = false;   // teardown events must not retry the connect
    esp_wifi_stop();
}

bool wifi_get_sta_ip(char *out, size_t out_sz) {
    if (!s_netif) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) return false;
    esp_ip4addr_ntoa(&ip.ip, out, out_sz);
    return true;
}

int wifi_rssi(void) {
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

esp_err_t wifi_sync_ntp(void) {
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&c) != ESP_OK) return ESP_FAIL;
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000));
    if (err != ESP_OK) ESP_LOGW(TAG, "NTP sync timeout");
    else ESP_LOGI(TAG, "NTP synced");
    return err;
}
