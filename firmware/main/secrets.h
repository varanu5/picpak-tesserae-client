// secrets.h — LOCAL dev credentials (git-ignored). Edit the WiFi lines below.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// >>> Optional dev shortcut: fill these to auto-connect without provisioning.
// Leave blank for a clean/publishable build — the device then boots straight
// into the captive portal and gets WiFi + server from provisioning (NVS). <<<
#define WIFI_DEFAULT_SSID        ""
#define WIFI_DEFAULT_PASS        ""

#define REST_DEFAULT_SERVER_URL  ""
