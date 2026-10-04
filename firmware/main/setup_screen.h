// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdint.h>
#include <stdbool.h>

void setup_screen_network(uint8_t *fb, const char *ssid);

bool setup_screen_wifi_qr(uint8_t *fb, const char *ssid, const char *password);
