// provisioning.h — SoftAP captive portal for WiFi/server provisioning (M4).
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_err.h"

// Brings up SoftAP "Tesserae-Setup" + DNS hijack + HTTP form; blocks until the
// user submits (persisted via config_store) or PROVISION_PORTAL_TIMEOUT_S fires.
// ESP_OK if creds were saved, ESP_ERR_TIMEOUT otherwise.
esp_err_t provisioning_run_blocking(void);
