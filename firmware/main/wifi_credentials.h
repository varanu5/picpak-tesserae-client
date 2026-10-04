// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool wifi_credentials_valid(const char *ssid, const char *pass) {
    if (!ssid || !pass) return false;
    size_t ns = strlen(ssid), np = strlen(pass);
    if (!ns || ns > 32 || np > 64 || (np && np < 8)) return false;
    if (np == 64) {
        for (size_t i = 0; i < np; i++) {
            char c = pass[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        }
    }
    return true;
}

static inline bool wifi_credentials_copy(uint8_t ssid_out[32], uint8_t pass_out[64],
                                         const char *ssid, const char *pass) {
    if (!wifi_credentials_valid(ssid, pass)) return false;
    memset(ssid_out, 0, 32);
    memset(pass_out, 0, 64);
    memcpy(ssid_out, ssid, strlen(ssid));
    memcpy(pass_out, pass, strlen(pass));
    return true;
}
