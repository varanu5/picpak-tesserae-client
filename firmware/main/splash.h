// splash.h — provisioning splash screens painted to the panel.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_err.h"

esp_err_t splash_show_setup(void);   // logo + AP name/password (entering provisioning)
esp_err_t splash_show_paired(void);  // "Connected — waiting for first frame" (post-submit, once)
esp_err_t splash_show_lowbatt(void); // "Battery low — please charge" (low-battery gate)
