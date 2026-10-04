// SPDX-License-Identifier: AGPL-3.0-or-later
#include "setup_identity.h"
#include "setup_screen.h"
#include "setup_font.h"
#include "board.h"
#include "esp_mac.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t device_mac[6] = {0xb0, 0xa6, 0x04, 0x53, 0x27, 0xcc};
static esp_err_t mac_result = ESP_OK;

esp_err_t esp_read_mac(uint8_t *mac, int type) {
    assert(type == ESP_MAC_WIFI_STA);
    memcpy(mac, device_mac, 6);
    return mac_result;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    char ssid[SETUP_SSID_CAPACITY];
    assert(setup_ap_ssid(ssid) == ESP_OK);
    assert(strcmp(ssid, "Tesserae-Setup-27CC") == 0);
    device_mac[4] = 0;
    device_mac[5] = 1;
    assert(setup_ap_ssid(ssid) == ESP_OK);
    assert(strcmp(ssid, "Tesserae-Setup-0001") == 0);
    device_mac[4] = 0xff;
    device_mac[5] = 0xff;
    assert(setup_ap_ssid(ssid) == ESP_OK);
    assert(strcmp(ssid, "Tesserae-Setup-FFFF") == 0);
    mac_result = ESP_FAIL;
    assert(setup_ap_ssid(ssid) == ESP_FAIL);
    assert(ssid[0] == '\0');

    unsigned max_digit = 0, prefix_width = 0;
    const char *prefix = "Tesserae-Setup-";
    for (const char *c = prefix; *c; c++) {
        unsigned matched = 0;
        for (size_t i = 0; i < sizeof(setup_glyphs) / sizeof(setup_glyphs[0]); i++)
            if (setup_glyphs[i].ch == *c) {
                prefix_width += setup_glyphs[i].advance;
                matched++;
            }
        assert(matched == 1);
    }
    for (const char *c = "0123456789ABCDEF"; *c; c++) {
        unsigned matched = 0;
        for (size_t i = 0; i < sizeof(setup_glyphs) / sizeof(setup_glyphs[0]); i++)
            if (setup_glyphs[i].ch == *c) {
                if (setup_glyphs[i].advance > max_digit) max_digit = setup_glyphs[i].advance;
                matched++;
            }
        assert(matched == 1);
    }
    assert(prefix_width + 4 * max_digit <= EPD_W - 152 - 12);

    uint8_t original[EPD_FB_BYTES], guarded[EPD_FB_BYTES + 2];
    FILE *input = fopen(argv[1], "rb");
    assert(input);
    assert(fread(original, 1, sizeof original, input) == sizeof original);
    fclose(input);
    memset(guarded, 0xa5, sizeof guarded);
    uint8_t *fb = guarded + 1;
    memcpy(fb, original, sizeof original);
    setup_screen_network(fb, "Tesserae-Setup-27CC");
    assert(guarded[0] == 0xa5 && guarded[EPD_FB_BYTES + 1] == 0xa5);
    unsigned changed = 0;
    for (int y = 0; y < EPD_H; y++)
        for (int x = 0; x < EPD_W / 4; x++) {
            size_t at = (size_t)(EPD_H - 1 - y) * (EPD_W / 4) + x;
            if (y < 134 || y >= 170 || x < 152 / 4) assert(fb[at] == original[at]);
            else changed += fb[at] != original[at];
        }
    assert(changed > 0);
    uint8_t before_qr[EPD_FB_BYTES];
    memcpy(before_qr, fb, sizeof before_qr);
    assert(!setup_screen_wifi_qr(fb, "", "tesserae"));
    assert(memcmp(before_qr, fb, sizeof before_qr) == 0);
    char oversized[224];
    memset(oversized, 'X', sizeof oversized - 1);
    oversized[sizeof oversized - 1] = '\0';
    assert(!setup_screen_wifi_qr(fb, oversized, "tesserae"));
    assert(memcmp(before_qr, fb, sizeof before_qr) == 0);
    assert(setup_screen_wifi_qr(fb, "Tesserae-Setup-27CC", "tesserae"));
    assert(guarded[0] == 0xa5 && guarded[EPD_FB_BYTES + 1] == 0xa5);
    for (int y = 0; y < EPD_H; y++)
        for (int x = 0; x < EPD_W; x++) {
            size_t at = (size_t)(EPD_H - 1 - y) * (EPD_W / 4) + x / 4;
            unsigned shift = 6 - 2 * (x % 4);
            unsigned pixel = (fb[at] >> shift) & 3;
            if (x < 15 || x >= 138 || y < 89 || y >= 212)
                assert(pixel == ((before_qr[at] >> shift) & 3));
            else if (x < 27 || x >= 126 || y < 101 || y >= 200)
                assert(pixel == 1);
            else assert(pixel == 0 || pixel == 1);
        }
    FILE *output = fopen(argv[2], "wb");
    assert(output);
    assert(fwrite(fb, 1, EPD_FB_BYTES, output) == EPD_FB_BYTES);
    fclose(output);
    // Replacing a long name must clear all pixels from the previous name.
    memcpy(fb, before_qr, EPD_FB_BYTES);
    setup_screen_network(fb, "Tesserae-Setup-0001");
    setup_screen_network(original, "Tesserae-Setup-0001");
    assert(memcmp(fb, original, EPD_FB_BYTES) == 0);
    puts("Setup identity and screen checks passed");
}
