// epd_driver.h — PicPak UC81xx-class e-paper SPI driver
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include "esp_err.h"
#include <stdint.h>

// Refresh waveform modes. Numeric values are the stored NVS values (see
// config_get/set_waveform) AND the provision-portal radio order — do NOT
// renumber. Faster waveforms trade color margin for speed; native is the
// panel's built-in fallback.
typedef enum {
    EPD_WAVE_5S     = 0,   // vendor fast LUT (~5 s); fastest, slightly softer colour
    EPD_WAVE_10S    = 1,   // vendor balanced LUT (~10 s)
    EPD_WAVE_NATIVE = 2,   // panel built-in MTP (~13-22 s); slowest, fullest colour
} epd_waveform_t;

esp_err_t epd_init(void);                 // SPI + GPIO, reset, run init sequence
esp_err_t epd_display(const uint8_t *fb); // load 30,000 bytes + refresh, wait BUSY
esp_err_t epd_sleep(void);                // panel deep sleep

// Complete one display cycle. Returns ESP_OK only after refresh and panel sleep
// succeed. On failure, attempts bounded shutdown and preserves the original error.
// Serialize calls; the driver owns one shared panel and SPI device.
esp_err_t epd_present(const uint8_t *fb);
