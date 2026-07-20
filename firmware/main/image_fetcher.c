// image_fetcher.c — download a frame URL into a caller buffer.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "image_fetcher.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "fetch";

int image_fetch(const char *url, uint8_t *buf, size_t buf_sz) {
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 20000 };
    // Same conditional as rest_handler: CA bundle only on https URLs.
    if (strncmp(url, "https://", 8) == 0)
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); return -1; }
    esp_http_client_fetch_headers(c);

    int status = esp_http_client_get_status_code(c);
    if (status < 200 || status > 299) {
        ESP_LOGE(TAG, "HTTP %d; not reading body", status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return -1;
    }

    int total = 0, r;
    while (total < (int)buf_sz &&
           (r = esp_http_client_read(c, (char *)buf + total, buf_sz - total)) > 0) {
        total += r;
    }
    // Oversize probe: with the buffer full, one more successful read means the
    // body is bigger than buf_sz — without this, a 40 KB response would read as
    // exactly buf_sz bytes and paint garbage. Works for chunked responses too,
    // where the Content-Length header is absent.
    if (total == (int)buf_sz) {
        char extra;
        if (esp_http_client_read(c, &extra, 1) > 0) {
            ESP_LOGE(TAG, "body exceeds %d bytes; rejecting", (int)buf_sz);
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            return -1;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    ESP_LOGI(TAG, "fetched %d bytes (HTTP %d)", total, status);
    return total;
}
