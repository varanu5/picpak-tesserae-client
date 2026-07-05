// secrets.example.h — copy to secrets.h (git-ignored) and fill in for dev.
// Lets you flash a board straight into REST mode without the captive portal
// (which arrives in M4). NVS values, once set, take precedence over these.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#define WIFI_DEFAULT_SSID        "your-wifi-ssid"
#define WIFI_DEFAULT_PASS        "your-wifi-password"

// Base URL of the Tesserae server (or the local mock), no trailing slash,
// no /api/v1. Example for the mock on your Mac: "http://192.168.1.50:8799"
#define REST_DEFAULT_SERVER_URL  "http://192.168.1.50:8799"
