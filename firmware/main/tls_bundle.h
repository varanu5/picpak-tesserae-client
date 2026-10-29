// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_err.h"
// Trusted-root bundle with candidate CA lookup for cross-signed chains.
esp_err_t tls_bundle_attach(void *config);
