// Never saves candidate Wi-Fi credentials or touches the server registration.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ble_wifi.h"
#include "wifi_credentials.h"
#include "defaults.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

#define CONNECTED BIT0
#define FAILED BIT1
#define SCANNED BIT2
#define STOPPED BIT3
static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_wifi_handler, s_ip_handler;
static bool s_initialized, s_started;
static bool s_driver_initialized, s_wifi_registered, s_ip_registered, s_owns_loop;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_events, CONNECTED);
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        xEventGroupSetBits(s_events, FAILED);
    else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE)
        xEventGroupSetBits(s_events, SCANNED);
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP)
        xEventGroupSetBits(s_events, STOPPED);
}

static esp_err_t initialize(void) {
    if (s_initialized) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_owns_loop = err == ESP_OK;
    s_events = xEventGroupCreate();
    if (!s_events) { err = ESP_ERR_NO_MEM; goto failed; }
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) { err = ESP_ERR_NO_MEM; goto failed; }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) goto failed;
    s_driver_initialized = true;
    // A failed test must never replace saved Wi-Fi driver configuration.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_wifi_handler);
    if (err == ESP_OK) s_wifi_registered = true;
    if (err == ESP_OK) err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, &s_ip_handler);
    if (err == ESP_OK) s_ip_registered = true;
    if (err == ESP_OK) s_initialized = true;
failed:
    if (err != ESP_OK) ble_wifi_stop();
    return err;
}

static esp_err_t start(void) {
    esp_err_t err = initialize();
    if (err != ESP_OK || s_started) return err;
    err = esp_wifi_start();
    if (err == ESP_OK) {
        s_started = true;
        esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);
    }
    return err;
}

static EventBits_t wait_bits(EventBits_t mask, unsigned timeout_ms,
                             ble_wifi_cancelled_t cancelled) {
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < end && !cancelled()) {
        EventBits_t bits = xEventGroupWaitBits(s_events, mask, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(100));
        if (bits & mask) return bits;
    }
    return 0;
}

esp_err_t ble_wifi_scan(wifi_network_t *out, size_t cap, size_t *count,
                       ble_wifi_cancelled_t cancelled) {
    *count = 0;
    esp_err_t err = start();
    if (err != ESP_OK) return err;
    xEventGroupClearBits(s_events, SCANNED);
    wifi_scan_config_t scan = { .show_hidden = false };
    err = esp_wifi_scan_start(&scan, false);
    if (err != ESP_OK) return err;
    if (!(wait_bits(SCANNED, 10000, cancelled) & SCANNED)) {
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        return ESP_ERR_TIMEOUT;
    }
    wifi_ap_record_t records[WIFI_SCAN_MAX_NETWORKS];
    uint16_t n = cap < WIFI_SCAN_MAX_NETWORKS ? (uint16_t)cap : WIFI_SCAN_MAX_NETWORKS;
    err = esp_wifi_scan_get_ap_records(&n, records);
    if (err != ESP_OK) return err;
    for (uint16_t i = 0; i < n; i++) {
        memcpy(out[i].ssid, records[i].ssid, 32);
        out[i].ssid[32] = '\0';
        out[i].rssi = records[i].rssi;
        out[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
    }
    *count = n;
    return ESP_OK;
}

esp_err_t ble_wifi_connect(const char *ssid, const char *password,
                          ble_wifi_cancelled_t cancelled) {
    esp_err_t err = start();
    if (err != ESP_OK) return err;
    // Wait for the old station's stop event before testing a different SSID.
    // A fixed delay could mistake an old DHCP/disconnect event for this attempt.
    xEventGroupClearBits(s_events, STOPPED);
    err = esp_wifi_stop();
    if (err != ESP_OK) return err;
    s_started = false;
    if (!(wait_bits(STOPPED, 2000, cancelled) & STOPPED)) return ESP_ERR_TIMEOUT;
    wifi_config_t cfg = {0};
    if (!wifi_credentials_copy(cfg.sta.ssid, cfg.sta.password, ssid, password))
        return ESP_ERR_INVALID_ARG;
    size_t ssid_len = strlen(ssid);
    cfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof cfg);
    if (err != ESP_OK || cancelled()) return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    xEventGroupClearBits(s_events, CONNECTED | FAILED);
    err = start();
    if (err != ESP_OK) return err;
    err = esp_wifi_connect();
    if (err != ESP_OK) return err;
    EventBits_t bits = wait_bits(CONNECTED | FAILED, 15000, cancelled);
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    bool matches = esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
        strnlen((const char *)ap.ssid, sizeof ap.ssid) == ssid_len &&
        memcmp(ap.ssid, ssid, ssid_len) == 0 &&
        esp_netif_get_ip_info(s_netif, &ip) == ESP_OK && ip.ip.addr != 0;
    if (!(bits & CONNECTED) || !matches || cancelled()) {
        esp_wifi_disconnect();
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool ble_wifi_get_ip(char *out, size_t cap) {
    if (!s_netif) return false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK || info.ip.addr == 0) return false;
    return esp_ip4addr_ntoa(&info.ip, out, cap) != NULL;
}

int ble_wifi_rssi(void) {
    wifi_ap_record_t ap;
    return s_started && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void ble_wifi_stop(void) {
    if (s_started) { esp_wifi_stop(); s_started = false; }
    if (s_wifi_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_registered = false;
    }
    if (s_ip_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_registered = false;
    }
    if (s_driver_initialized) {
        esp_wifi_deinit();
        s_driver_initialized = false;
    }
    s_initialized = false;
    if (s_netif) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }
    if (s_events) { vEventGroupDelete(s_events); s_events = NULL; }
    if (s_owns_loop) { esp_event_loop_delete_default(); s_owns_loop = false; }
}
