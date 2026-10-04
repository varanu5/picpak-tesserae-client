// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "setup_identity.h"
#include "defaults.h"
#include "esp_mac.h"
#include <stdio.h>

_Static_assert(sizeof(PROVISION_AP_SSID) + 5 <= SETUP_SSID_CAPACITY,
               "Setup network name exceeds the WiFi limit");

esp_err_t setup_ap_ssid(char ssid[SETUP_SSID_CAPACITY]) {
    uint8_t mac[6];
    ssid[0] = '\0';
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) return err;
    snprintf(ssid, SETUP_SSID_CAPACITY, "%s-%02X%02X", PROVISION_AP_SSID,
             (unsigned)mac[4], (unsigned)mac[5]);
    return ESP_OK;
}
