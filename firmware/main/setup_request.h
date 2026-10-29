// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdbool.h>
#include "display_mode.h"
typedef struct {
    char ssid[33], pass[65], server[160], id[64], pair[16];
    char relay_url[160], relay_code[40];
    char mqtt_uri[160], mqtt_user[64], mqtt_pass[64];
    bool mqtt;
    bool relay;
    bool cloud;
    bool has_display_mode;
    display_mode_t display_mode;
} setup_request_t;
// Returns a fixed user-facing error, or NULL after validation.
const char *setup_request_parse(const char *body, const char *nonce,
                               const char *old_ssid, const char *old_pass,
                               const char *old_id, setup_request_t *out);
const char *setup_display_mode_parse(const char *body, const char *nonce, display_mode_t *out);
const char *setup_request_check_cloud_pairing(const setup_request_t *request,
    const char *old_server, const char *old_id, bool paired);
