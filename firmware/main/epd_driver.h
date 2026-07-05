// epd_driver.h — PicPak UC81xx-class e-paper SPI driver
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t epd_init(void);                 // SPI + GPIO, reset, run init sequence
void      epd_display(const uint8_t *fb); // load 30,000 bytes + refresh, wait BUSY
void      epd_sleep(void);                // panel deep sleep
