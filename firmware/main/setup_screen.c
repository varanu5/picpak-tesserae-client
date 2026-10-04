// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "setup_screen.h"
#include "setup_font.h"
#include "qrcodegen.h"
#include "board.h"
#include <stddef.h>
#include <string.h>

void setup_screen_network(uint8_t *fb, const char *ssid) {
    // Replace the network row in the embedded setup screen.
    for (int y = 134; y < 170; y++)
        memset(fb + (EPD_H - 1 - y) * (EPD_W / 4) + 152 / 4,
               0x55, (EPD_W - 152) / 4);
    int x = 152;
    for (; *ssid; ssid++) {
        for (size_t i = 0; i < sizeof(setup_glyphs) / sizeof(setup_glyphs[0]); i++) {
            if (setup_glyphs[i].ch != *ssid) continue;
            for (int row = 0; row < 24; row++)
                for (int col = 0; col < 24 && x + col < EPD_W; col++) {
                    if (!(setup_glyphs[i].rows[row] & (1u << col))) continue;
                    size_t at = (size_t)(EPD_H - 1 - (136 + row)) * (EPD_W / 4)
                                + (x + col) / 4;
                    fb[at] &= (uint8_t)~(3u << (6 - 2 * ((x + col) % 4)));
                }
            x += setup_glyphs[i].advance;
            break;
        }
    }
}

static bool append_wifi_field(char *payload, size_t capacity, size_t *used, const char *value) {
    for (; *value; value++) {
        if (strchr("\\;,:\"", *value)) {
            if (*used + 1 >= capacity) return false;
            payload[(*used)++] = '\\';
        }
        if (*used + 1 >= capacity) return false;
        payload[(*used)++] = *value;
    }
    payload[*used] = '\0';
    return true;
}

bool setup_screen_wifi_qr(uint8_t *fb, const char *ssid, const char *password) {
    if (!fb || !ssid || !password || !ssid[0]) return false;
    char payload[224] = "WIFI:T:WPA;S:";
    size_t used = strlen(payload);
    if (!append_wifi_field(payload, sizeof payload, &used, ssid)) return false;
    if (used + 3 >= sizeof payload) return false;
    memcpy(payload + used, ";P:", 3);
    used += 3;
    if (!append_wifi_field(payload, sizeof payload, &used, password)) return false;
    if (used + 2 >= sizeof payload) return false;
    memcpy(payload + used, ";;", 3);

    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(4)];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(4)];
    if (!qrcodegen_encodeText(payload, tmp, qr, qrcodegen_Ecc_MEDIUM,
                              1, 4, qrcodegen_Mask_AUTO, true)) return false;
    int modules = qrcodegen_getSize(qr);
    int width = (modules + 8) * 3;
    int left = 12 + (129 - width) / 2;
    int top = 150 - width / 2;
    // Use whole pixels and a white border four modules wide.
    for (int y = 0; y < width; y++)
        for (int x = 0; x < width; x++) {
            int mx = x / 3 - 4;
            int my = y / 3 - 4;
            bool black = mx >= 0 && my >= 0 && mx < modules && my < modules
                         && qrcodegen_getModule(qr, mx, my);
            int px = left + x;
            size_t at = (size_t)(EPD_H - 1 - (top + y)) * (EPD_W / 4) + px / 4;
            unsigned shift = 6 - 2 * (px % 4);
            fb[at] = (uint8_t)((fb[at] & ~(3u << shift)) | ((black ? 0u : 1u) << shift));
        }
    return true;
}
