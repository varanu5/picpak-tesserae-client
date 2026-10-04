// splash.c — paint embedded 400x300 2bpp splash blobs to the panel.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "splash.h"
#include "config_store.h"
#include "setup_identity.h"
#include "setup_screen.h"
#include "defaults.h"
#include "framebuf.h"
#include <string.h>
#include "board.h"
#include "epd_driver.h"
#include "esp_log.h"
#include <stdint.h>
#include <stddef.h>

static const char *TAG = "splash";

// Symbols injected by CMake EMBED_FILES (see CMakeLists.txt).
extern const uint8_t _binary_splash_setup_bin_start[];
extern const uint8_t _binary_splash_setup_bin_end[];
extern const uint8_t _binary_splash_paired_bin_start[];
extern const uint8_t _binary_splash_paired_bin_end[];
extern const uint8_t _binary_splash_lowbatt_bin_start[];
extern const uint8_t _binary_splash_lowbatt_bin_end[];
extern const uint8_t _binary_splash_revoked_bin_start[];
extern const uint8_t _binary_splash_revoked_bin_end[];

static esp_err_t paint(const uint8_t *start, const uint8_t *end, const char *label) {
    size_t len = (size_t)(end - start);
    if (len != EPD_FB_BYTES) {
        ESP_LOGE(TAG, "%s blob is %u bytes, expected %u; refusing to paint",
                 label, (unsigned)len, (unsigned)EPD_FB_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "painting %s splash (~13-22 s)...", label);
    esp_err_t err = epd_present(start);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "%s splash failed: %s", label, esp_err_to_name(err));
    return err;
}

esp_err_t splash_show_setup(void) {
    char ssid[SETUP_SSID_CAPACITY];
    esp_err_t err = setup_ap_ssid(ssid);
    if (err != ESP_OK) return err;
    if (_binary_splash_setup_bin_end - _binary_splash_setup_bin_start != EPD_FB_BYTES)
        return ESP_ERR_INVALID_SIZE;
    uint8_t *fb = framebuf();
    memcpy(fb, _binary_splash_setup_bin_start, EPD_FB_BYTES);
    setup_screen_network(fb, ssid);
    if (!setup_screen_wifi_qr(fb, ssid, PROVISION_AP_PASS))
        ESP_LOGW(TAG, "Setup QR unavailable, use the displayed WiFi details");
    esp_err_t painted = paint(fb, fb + EPD_FB_BYTES, "setup");
    if (painted == ESP_OK) config_set_lowbatt_screen(false);
    return painted;
}
esp_err_t splash_show_paired(void) {
    esp_err_t err = paint(_binary_splash_paired_bin_start, _binary_splash_paired_bin_end, "paired");
    if (err == ESP_OK) config_set_lowbatt_screen(false);
    return err;
}
esp_err_t splash_show_lowbatt(void) {
    // Save the recovery intent before the panel can replace the current photo.
    esp_err_t err = config_set_lowbatt_screen(true);
    if (err != ESP_OK) return err;
    config_clear_frame_ref();
    return paint(_binary_splash_lowbatt_bin_start, _binary_splash_lowbatt_bin_end, "lowbatt");
}
esp_err_t splash_show_revoked(void) {
    esp_err_t err = paint(_binary_splash_revoked_bin_start, _binary_splash_revoked_bin_end, "revoked");
    if (err == ESP_OK) config_set_lowbatt_screen(false);
    return err;
}
