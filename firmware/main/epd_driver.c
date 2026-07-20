// epd_driver.c — PicPak UC81xx-class e-paper SPI driver
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "epd_driver.h"
#include "epd_init_seq.h"
#include "board.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "epd";
static spi_device_handle_t s_spi;
static bool s_spi_ready = false;   // SPI bus set up once per boot; epd_init re-callable

// UC81xx BUSY is active-low: panel is busy while the line reads 0.
static void epd_wait_busy(void) {
    int guard = 0;
    while (gpio_get_level(PIN_EPD_BUSY) == 0 && guard++ < 4000)
        vTaskDelay(pdMS_TO_TICKS(10));   // up to ~40 s guard
    if (guard >= 4000) ESP_LOGW(TAG, "wait_busy timeout");
}

static void epd_cmd(uint8_t c) {
    gpio_set_level(PIN_EPD_DC, 0);       // DC low = command
    spi_transaction_t t = { .length = 8, .tx_buffer = &c };
    spi_device_polling_transmit(s_spi, &t);
}

static void epd_data(const uint8_t *d, int n) {
    if (n <= 0) return;
    gpio_set_level(PIN_EPD_DC, 1);       // DC high = data
    for (int off = 0; off < n; off += 512) {        // 512-byte chunks
        int chunk = (n - off > 512) ? 512 : (n - off);
        spi_transaction_t t = { .length = 8 * chunk, .tx_buffer = d + off };
        spi_device_polling_transmit(s_spi, &t);
    }
}

static void epd_reset(void) {
    gpio_set_level(PIN_EPD_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_EPD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_EPD_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
}

esp_err_t epd_init(void) {
    gpio_config_t out = { .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_EPD_DC) | (1ULL << PIN_EPD_RST) };
    gpio_config(&out);
    gpio_config_t in = { .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_EPD_BUSY) };
    gpio_config(&in);

    if (!s_spi_ready) {
        spi_bus_config_t bus = {
            .mosi_io_num = PIN_EPD_MOSI, .miso_io_num = PIN_EPD_MISO,
            .sclk_io_num = PIN_EPD_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 512,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
        spi_device_interface_config_t dev = {
            .clock_speed_hz = EPD_SPI_HZ, .mode = 0,
            .spics_io_num = PIN_EPD_CS, .queue_size = 4,
        };
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));
        s_spi_ready = true;
    }

    epd_reset();
    epd_wait_busy();
    // Run the init table. Iterate by size (0xFF is a valid command, not a sentinel).
    const uint8_t *seq = EPD_INIT_SPECIFIC;      // shipping panels report EPD ID 06 04
    size_t n = sizeof(EPD_INIT_SPECIFIC);
    for (size_t i = 0; i < n; ) {
        uint8_t cmd = seq[i++];
        uint8_t len = seq[i++];
        epd_cmd(cmd);
        epd_data(&seq[i], len);
        i += len;
    }
    ESP_LOGI(TAG, "init done");
    return ESP_OK;
}

void epd_display(const uint8_t *fb) {
    epd_cmd(0x10);                       // DTM1: framebuffer load (confirm opcode)
    epd_data(fb, EPD_FB_BYTES);
    epd_cmd(0x04); epd_wait_busy();      // Power ON
    epd_cmd(0x12); epd_wait_busy();      // Display Refresh
    ESP_LOGI(TAG, "display done");
}

void epd_sleep(void) {
    epd_cmd(0x02); epd_wait_busy();      // Power OFF
    epd_cmd(0x07);                       // Deep Sleep
}
