// provisioning.h — SoftAP captive portal for WiFi/server provisioning.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_err.h"

// Brings up SoftAP "Tesserae-Setup" + DNS hijack + HTTP form; blocks until the
// user submits (persisted via config_store) or the idle timeout fires (only
// counted while no client is connected). `note` (may be NULL) is a constant
// error string shown as a banner on the form — used when re-entering the
// portal because the just-saved settings didn't work (e.g. server unreachable).
// ESP_OK if creds were saved, ESP_ERR_TIMEOUT otherwise.
esp_err_t provisioning_run_blocking(const char *note);
