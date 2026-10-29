// splash.h — provisioning splash screens painted to the panel.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_err.h"

esp_err_t splash_show_setup(void);   // logo + AP name/password (entering provisioning)
esp_err_t splash_show_paired(void);  // Confirmed connection, waiting for an image.
esp_err_t splash_show_lowbatt(void); // "Battery low — please charge" (low-battery gate)
esp_err_t splash_show_revoked(void); // Removed connection, hold the button for setup.

esp_err_t splash_show_registration(void); // Open Tesserae Settings > Devices.
