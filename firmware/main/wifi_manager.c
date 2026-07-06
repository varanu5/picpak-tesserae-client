// wifi_manager.c — STA connect + NTP.
// SPDX-License-Identifier: AGPL-3.0-or-later
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
#include "esp_log.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_eg;
#define BIT_CONN BIT0
#define BIT_FAIL BIT1
static int s_retries;
static esp_netif_t *s_netif;

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < WIFI_CONNECT_RETRIES) { s_retries++; esp_wifi_connect(); }
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
    // "tesserae-picpak" instead of the ESP-IDF default "espressif".
    esp_err_t herr = esp_netif_set_hostname(s_netif, WIFI_HOSTNAME);
    if (herr != ESP_OK) ESP_LOGW(TAG, "set_hostname: %s", esp_err_to_name(herr));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // PicPak brownout guard: cap TX power before the radio transmits.
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);

    ESP_LOGI(TAG, "connecting to '%s'", ssid);
    EventBits_t b = xEventGroupWaitBits(s_eg, BIT_CONN | BIT_FAIL, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (b & BIT_CONN) { ESP_LOGI(TAG, "connected"); return ESP_OK; }
    ESP_LOGW(TAG, "connect failed/timeout");
    return ESP_FAIL;
}

void wifi_stop(void) {
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
