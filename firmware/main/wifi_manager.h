// wifi_manager.h — STA connect + NTP.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

esp_err_t wifi_start_sta(void);   // connect with config_store creds (blocking, retried)
void      wifi_stop(void);
bool      wifi_get_sta_ip(char *out, size_t out_sz);
int       wifi_rssi(void);
esp_err_t wifi_sync_ntp(void);    // best-effort SNTP, waits a few seconds
