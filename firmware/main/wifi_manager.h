// wifi_manager.h — STA connect + NTP.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

esp_err_t wifi_start_sta(void);   // connect with config_store creds (blocking, retried)
void      wifi_stop(void);

// After a failed wifi_start_sta: classify the latched failure reason. Both
// drive the one-shot portal re-entry (with distinct banners) on the first boot
// after provisioning; transient failures match neither and stay silent.
//   bad_password — auth failure / handshake timeout signatures.
//   no_ap        — no joinable AP by that name (SSID typo, or the name exists
//                  with incompatible security, e.g. password set for an open AP).
bool      wifi_fail_looks_like_bad_password(void);
bool      wifi_fail_looks_like_no_ap(void);
bool      wifi_get_sta_ip(char *out, size_t out_sz);
int       wifi_rssi(void);
esp_err_t wifi_sync_ntp(void);    // best-effort SNTP, waits a few seconds. Off the REST
                                  // wake path (see main.c); MQTT mode runs it when the
                                  // clock is implausible (its only clock source).
