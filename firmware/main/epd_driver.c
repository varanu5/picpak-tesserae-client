// epd_driver.c — PicPak e-paper SPI driver
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "epd_driver.h"
#include "epd_init_seq.h"
#include "epd_lut_5s.h"
#include "epd_lut_10s.h"
#include "config_store.h"
#include "defaults.h"
#include "board.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "epd";
static spi_device_handle_t s_spi;
static bool s_spi_ready;
static bool s_initialized;

#define EPD_INIT_TIMEOUT_MS 5000
#define EPD_POWER_TIMEOUT_MS 50000
#define EPD_TRY(call) do { esp_err_t err_ = (call); if (err_ != ESP_OK) return err_; } while (0)

static void epd_delay_ms(uint32_t ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

// BUSY is active-low. Keep a settling interval after the controller becomes ready.
static esp_err_t epd_wait_busy(uint32_t timeout_ms) {
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (gpio_get_level(PIN_EPD_BUSY) == 0) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW(TAG, "BUSY timeout (%lu ms)", (unsigned long)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        epd_delay_ms(10);
    }
    epd_delay_ms(100);
    return ESP_OK;
}

static esp_err_t epd_cmd(uint8_t c) {
    EPD_TRY(gpio_set_level(PIN_EPD_DC, 0));
    spi_transaction_t t = { .length = 8, .tx_buffer = &c };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t epd_data(const uint8_t *d, int n) {
    if (n <= 0) return ESP_OK;
    EPD_TRY(gpio_set_level(PIN_EPD_DC, 1));
    for (int off = 0; off < n; off += 512) {
        int chunk = (n - off > 512) ? 512 : (n - off);
        spi_transaction_t t = { .length = 8 * chunk, .tx_buffer = d + off };
        EPD_TRY(spi_device_polling_transmit(s_spi, &t));
    }
    return ESP_OK;
}

static esp_err_t epd_reset(void) {
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1)); epd_delay_ms(20);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 0)); epd_delay_ms(20);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1)); epd_delay_ms(20);
    return ESP_OK;
}

static esp_err_t epd_send(uint8_t cmd, const uint8_t *data, int n) {
    EPD_TRY(epd_cmd(cmd));
    return epd_data(data, n);
}

// 528 waveform bytes plus seven timing bytes. Upload the timing bytes first,
// then transpose the 11 x 48 source matrix into 48 groups of 11 bytes.
static esp_err_t epd_init_vendor_lut(const uint8_t *L) {
    const uint8_t *TR = L + 0x210;
    EPD_TRY(epd_send(0x06, (const uint8_t[]){0x0F, 0x8B, 0x9C, 0x96}, 4));
    EPD_TRY(epd_send(0x50, (const uint8_t[]){0x37}, 1));
    EPD_TRY(epd_send(0x00, (const uint8_t[]){0x07, 0xA9}, 2));
    EPD_TRY(epd_wait_busy(EPD_INIT_TIMEOUT_MS));

    const uint8_t pwr[6] = {0x07, TR[0], TR[1], TR[3], TR[2], TR[4]};
    EPD_TRY(epd_send(0x01, pwr, 6));
    const uint8_t v82 = (uint8_t)(TR[5] - 0x80);
    EPD_TRY(epd_send(0x82, &v82, 1));
    const uint8_t pll = TR[6];
    EPD_TRY(epd_send(0x30, &pll, 1));
    EPD_TRY(epd_send(0x61, (const uint8_t[]){0x01, 0x90, 0x01, 0x2C}, 4));
    EPD_TRY(epd_wait_busy(EPD_INIT_TIMEOUT_MS));
    EPD_TRY(epd_send(0x62, (const uint8_t[]){0x62, 0x51}, 2));
    EPD_TRY(epd_send(0x65, (const uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4));
    EPD_TRY(epd_wait_busy(EPD_INIT_TIMEOUT_MS));
    EPD_TRY(epd_send(0xE7, (const uint8_t[]){0x96}, 1));
    EPD_TRY(epd_send(0xE9, (const uint8_t[]){0x01}, 1));
    EPD_TRY(epd_cmd(0x20));
    EPD_TRY(epd_data(TR, 7));
    for (int row = 0; row < 48; row++) {
        uint8_t line[11];
        for (int col = 0; col < 11; col++) line[col] = L[row + 48 * col];
        EPD_TRY(epd_data(line, 11));
    }
    ESP_LOGI(TAG, "init done (vendor LUT, PLL=0x%02X)", pll);
    return ESP_OK;
}

static esp_err_t epd_init_native(void) {
    const uint8_t *seq = EPD_INIT_NATIVE;
    // 0xFF is a valid command; iterate by array size, not a sentinel.
    for (size_t i = 0; i < sizeof(EPD_INIT_NATIVE); ) {
        uint8_t cmd = seq[i++];
        uint8_t len = seq[i++];
        EPD_TRY(epd_send(cmd, &seq[i], len));
        i += len;
    }
    ESP_LOGI(TAG, "init done (native full init)");
    return ESP_OK;
}

esp_err_t epd_init(void) {
    s_initialized = false;
    gpio_config_t out = { .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_EPD_DC) | (1ULL << PIN_EPD_RST) };
    EPD_TRY(gpio_config(&out));
    gpio_config_t in = { .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_EPD_BUSY) };
    EPD_TRY(gpio_config(&in));
    if (!s_spi_ready) {
        spi_bus_config_t bus = {
            .mosi_io_num = PIN_EPD_MOSI, .miso_io_num = PIN_EPD_MISO,
            .sclk_io_num = PIN_EPD_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 512,
        };
        EPD_TRY(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
        spi_device_interface_config_t dev = {
            .clock_speed_hz = EPD_SPI_HZ, .mode = 0,
            .spics_io_num = PIN_EPD_CS, .queue_size = 4,
        };
        esp_err_t err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
        if (err != ESP_OK) {
            spi_bus_free(SPI2_HOST);
            return err;
        }
        s_spi_ready = true;
    }
    EPD_TRY(epd_reset());
    EPD_TRY(epd_wait_busy(EPD_INIT_TIMEOUT_MS));
    // Preserve the selected NVS waveform and compile-time fallback.
    switch (config_get_waveform(DEFAULT_WAVEFORM)) {
    case EPD_WAVE_5S: EPD_TRY(epd_init_vendor_lut(EPD_LUT5S)); break;
    case EPD_WAVE_10S: EPD_TRY(epd_init_vendor_lut(EPD_LUT10S)); break;
    default: EPD_TRY(epd_init_native()); break;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t epd_display(const uint8_t *fb) {
    if (!fb) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    EPD_TRY(epd_send(0x10, fb, EPD_FB_BYTES));
    EPD_TRY(epd_cmd(0x04));
    epd_delay_ms(20);
    EPD_TRY(epd_wait_busy(EPD_POWER_TIMEOUT_MS));
    EPD_TRY(epd_send(0x12, (const uint8_t[]){0x00}, 1));
    epd_delay_ms(20);
    EPD_TRY(epd_wait_busy(EPD_POWER_TIMEOUT_MS));
    ESP_LOGI(TAG, "display done");
    return ESP_OK;
}

esp_err_t epd_sleep(void) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    EPD_TRY(epd_send(0x02, (const uint8_t[]){0x00}, 1));
    epd_delay_ms(20);
    EPD_TRY(epd_wait_busy(EPD_POWER_TIMEOUT_MS));
    EPD_TRY(epd_send(0x07, (const uint8_t[]){0xA5}, 1));
    s_initialized = false;
    ESP_LOGI(TAG, "panel deep sleep");
    return ESP_OK;
}

esp_err_t epd_present(const uint8_t *fb) {
    if (!fb) return ESP_ERR_INVALID_ARG;
    esp_err_t err = epd_init();
    if (err == ESP_OK) err = epd_display(fb);
    if (err == ESP_OK) err = epd_sleep();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display cycle failed: %s", esp_err_to_name(err));
        // One bounded recovery attempt. Reset/reinitialize before shutdown rather
        // than sending commands into a timed-out refresh or a partial SPI payload.
        // Never redraw or turn a failed presentation into a successful result.
        if (s_spi_ready) {
            esp_err_t cleanup = epd_init();
            if (cleanup == ESP_OK) cleanup = epd_sleep();
            if (cleanup != ESP_OK)
                ESP_LOGW(TAG, "panel cleanup failed: %s", esp_err_to_name(cleanup));
        }
    }
    return err;
}
