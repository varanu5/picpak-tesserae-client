// secrets.example.h — copy to secrets.h (git-ignored) and fill in for dev.
// Lets you flash a board straight into REST mode without the captive portal.
// NVS values, once set, take precedence over these.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once

#define WIFI_DEFAULT_SSID        "your-wifi-ssid"
#define WIFI_DEFAULT_PASS        "your-wifi-password"

// Base URL of the Tesserae server (or the local mock), no trailing slash,
// no /api/v1. Example for the mock on your Mac: "http://192.168.1.50:8799"
#define REST_DEFAULT_SERVER_URL  "http://192.168.1.50:8799"

// MQTT dev defaults (transport mode 0). Only read when the portal hasn't
// stored a broker in NVS. Scheme optional — "host:1883" gets mqtt:// prepended.
// #define MQTT_DEFAULT_URI   "mqtt://192.168.1.50:1883"
// #define MQTT_DEFAULT_USER  ""
// #define MQTT_DEFAULT_PASS  ""
