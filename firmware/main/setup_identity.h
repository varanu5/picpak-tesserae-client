// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_err.h"

#define SETUP_SSID_CAPACITY 33

// Use the station MAC so the suffix matches the device identity.
esp_err_t setup_ap_ssid(char ssid[SETUP_SSID_CAPACITY]);
