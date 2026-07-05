// image_fetcher.c — download a frame URL into a caller buffer.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "image_fetcher.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "fetch";

int image_fetch(const char *url, uint8_t *buf, size_t buf_sz) {
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 20000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); return -1; }
    esp_http_client_fetch_headers(c);

    int total = 0, r;
    while (total < (int)buf_sz &&
           (r = esp_http_client_read(c, (char *)buf + total, buf_sz - total)) > 0) {
        total += r;
    }
    int status = esp_http_client_get_status_code(c);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    ESP_LOGI(TAG, "fetched %d bytes (HTTP %d)", total, status);
    return (status == 200) ? total : -1;
}
