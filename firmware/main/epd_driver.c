// epd_driver.c — PicPak UC81xx-class e-paper SPI driver
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

static void epd_send(uint8_t cmd, const uint8_t *data, int n) {
    epd_cmd(cmd);
    epd_data(data, n);
}

// Vendor external-LUT init + waveform upload (SSD2683 P420E55). Works for any
// vendor LUT body (5s = LUT A, 10s = LUT B) because the 8-byte timing trailer at
// L[0x210] carries the per-waveform PWR / 0x82 / PLL values. Two things MUST be
// right or the panel hangs (BUSY never releases): the 0x06 BTST booster must run
// (else the HV rails never rise), and the 0x20 LUT upload must be prefixed with
// the 7-byte header (trailer bytes) so the controller has phase/frame counts —
// payload is 7 + 528 = 535 bytes.
static void epd_init_vendor_lut(const uint8_t *L) {
    const uint8_t *TR = L + 0x210;        // trailer; first 7 bytes = 0x20 header

    epd_send(0x06, (const uint8_t[]){0x0F, 0x8B, 0x9C, 0x96}, 4); // BTST booster
    epd_send(0x50, (const uint8_t[]){0x37}, 1);                   // CDI
    epd_send(0x00, (const uint8_t[]){0x07, 0xA9}, 2);            // PSR (0x80=ext LUT)
    vTaskDelay(pdMS_TO_TICKS(5));

    const uint8_t pwr[6] = { 0x07, TR[0], TR[1], TR[3], TR[2], TR[4] };
    // pwr[]: 0=flags 1=VGH/VGL 2=VDH 3=VDL 4=VDHR 5=+ (from the LUT trailer).
    epd_send(0x01, pwr, 6);                                       // PWR (trailer)
    const uint8_t v82 = (uint8_t)(TR[5] - 0x80);
    epd_send(0x82, &v82, 1);                                      // vendor
    const uint8_t pll = TR[6];
    epd_send(0x30, &pll, 1);                                      // PLL (5s=0x06,10s=0x03)

    epd_send(0x61, (const uint8_t[]){0x01, 0x90, 0x01, 0x2C}, 4); // TRES 400x300
    vTaskDelay(pdMS_TO_TICKS(5));
    epd_send(0x62, (const uint8_t[]){0x62, 0x51}, 2);            // vendor
    epd_send(0x65, (const uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4); // GSST
    vTaskDelay(pdMS_TO_TICKS(5));
    epd_send(0xE7, (const uint8_t[]){0x96}, 1);                  // vendor
    epd_send(0xE9, (const uint8_t[]){0x01}, 1);                  // vendor

    // LUT load: cmd 0x20, the 7-byte header (trailer), THEN 528 waveform bytes
    // streamed column-major (48 rows x 11, stride 48).
    epd_cmd(0x20);
    epd_data(TR, 7);                                              // 7-byte header
    for (int row = 0; row < 48; row++) {
        uint8_t line[11];
        for (int col = 0; col < 11; col++) line[col] = L[row + 48 * col];
        epd_data(line, 11);
    }
    ESP_LOGI(TAG, "init done (vendor LUT, PLL=0x%02X)", pll);
}

// Panel built-in "native MTP" waveform — the safe, slow default. Iterate by
// size (0xFF is a valid command byte here, not a sentinel).
static void epd_init_native(void) {
    const uint8_t *seq = EPD_INIT_NATIVE;        // full factory init — all panels
    size_t n = sizeof(EPD_INIT_NATIVE);
    for (size_t i = 0; i < n; ) {
        uint8_t cmd = seq[i++];
        uint8_t len = seq[i++];
        epd_cmd(cmd);
        epd_data(&seq[i], len);
        i += len;
    }
    ESP_LOGI(TAG, "init done (native full init)");
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

    // Dispatch on the selected refresh waveform (portal-set, NVS-persisted, with
    // the compile-time DEFAULT_WAVEFORM fallback). 5s/10s upload a vendor LUT;
    // native uses the panel's built-in MTP waveform.
    switch (config_get_waveform(DEFAULT_WAVEFORM)) {
    case EPD_WAVE_5S:
        ESP_LOGI(TAG, "waveform: 5s (vendor fast LUT)");
        epd_init_vendor_lut(EPD_LUT5S);
        break;
    case EPD_WAVE_10S:
        ESP_LOGI(TAG, "waveform: 10s (vendor balanced LUT)");
        epd_init_vendor_lut(EPD_LUT10S);
        break;
    default:
        ESP_LOGI(TAG, "waveform: native MTP");
        epd_init_native();
        break;
    }
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
    // Power OFF (0x02) carries a 0x00 parameter byte on this controller.
    uint8_t pof = 0x00;
    epd_cmd(0x02); epd_data(&pof, 1); epd_wait_busy();
    // Deep Sleep (0x07) requires the 0xA5 check-code parameter — the controller
    // ignores a bare 0x07 (a guard against an accidental sleep), so without the
    // check-code the panel never leaves standby and keeps drawing between wakes.
    uint8_t dslp = 0xA5;
    epd_cmd(0x07); epd_data(&dslp, 1);
    ESP_LOGI(TAG, "panel deep sleep");
}
