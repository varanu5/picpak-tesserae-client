// splash.c — paint embedded 400x300 2bpp splash blobs to the panel.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "splash.h"
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

static esp_err_t paint(const uint8_t *start, const uint8_t *end, const char *label) {
    size_t len = (size_t)(end - start);
    if (len != EPD_FB_BYTES) {
        ESP_LOGE(TAG, "%s blob is %u bytes, expected %u; refusing to paint",
                 label, (unsigned)len, (unsigned)EPD_FB_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = epd_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "epd_init failed (%d); skipping %s splash", err, label);
        return err;
    }
    ESP_LOGI(TAG, "painting %s splash (~13-22 s)...", label);
    epd_display(start);
    epd_sleep();
    return ESP_OK;
}

esp_err_t splash_show_setup(void) {
    return paint(_binary_splash_setup_bin_start, _binary_splash_setup_bin_end, "setup");
}
esp_err_t splash_show_paired(void) {
    return paint(_binary_splash_paired_bin_start, _binary_splash_paired_bin_end, "paired");
}
esp_err_t splash_show_lowbatt(void) {
    return paint(_binary_splash_lowbatt_bin_start, _binary_splash_lowbatt_bin_end, "lowbatt");
}
