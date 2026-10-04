// Exercise the real config_store.c against an in-memory NVS backend.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "config_store.h"
#include "setup_check.h"

typedef struct { unsigned ns; char key[16]; uint8_t data[512]; size_t len; } entry_t;
static entry_t entries[64];
static char namespaces[8][16];
static unsigned namespace_count, commits;
static int fail_commit;
static entry_t *lookup(unsigned ns, const char *key, int create) {
    for (size_t i = 0; i < 64; i++)
        if (entries[i].ns == ns && !strcmp(entries[i].key, key)) return &entries[i];
    if (create) for (size_t i = 0; i < 64; i++) if (!entries[i].ns) {
        entries[i].ns = ns; snprintf(entries[i].key, 16, "%s", key); return &entries[i];
    }
    return NULL;
}
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { memset(entries, 0, sizeof entries); return ESP_OK; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    for (unsigned i = 0; i < namespace_count; i++) if (!strcmp(ns, namespaces[i])) {
        *h = i + 1; return ESP_OK;
    }
    if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    assert(namespace_count < 8);
    snprintf(namespaces[namespace_count], 16, "%s", ns);
    *h = ++namespace_count; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; commits++; return fail_commit ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) {
    for (size_t i = 0; i < 64; i++) if (entries[i].ns == h) memset(&entries[i], 0, sizeof entries[i]);
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    entry_t *e = lookup(h, key, 0); if (!e) return ESP_ERR_NVS_NOT_FOUND;
    memset(e, 0, sizeof *e); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len) {
    entry_t *e = lookup(h, key, 1); assert(e && len <= sizeof e->data);
    memcpy(e->data, data, len); e->len = len; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *len) {
    entry_t *e = lookup(h, key, 0); if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) return ESP_FAIL;
    memcpy(data, e->data, e->len); *len = e->len; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *s) { return nvs_set_blob(h, k, s, strlen(s) + 1); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *s, size_t *n) { return nvs_get_blob(h, k, s, n); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { return nvs_set_blob(h, k, &v, sizeof v); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { size_t n = sizeof *v; return nvs_get_blob(h, k, v, &n); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { return nvs_set_blob(h, k, &v, sizeof v); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v) { size_t n = sizeof *v; return nvs_get_blob(h, k, v, &n); }

static void test_initial_connection_recovery(void) {
    for (unsigned transport = 0; transport < 3; transport++) {
        config_clear_relay();
        config_set_transport(transport == 0 ? 0 : 1);
        if (transport == 2) {
            config_set_relay_url("https://relay.test");
            config_set_relay_code("test-code");
        }
        config_set_wifi("setup-network", "setup-password");
        config_set_server_url("http://server.test");
        config_set_device_token("saved-token");

        // An update with saved settings does not arm setup recovery.
        assert(config_init() == ESP_OK);
        assert(!setup_should_reopen(config_take_paired_pending(), false, false));

        // Any initial WiFi failure reopens setup, including generic reason 205.
        config_set_paired_pending(true);
        assert(config_init() == ESP_OK);
        assert(setup_should_reopen(config_take_paired_pending(), false, false));
        for (unsigned wake = 0; wake < 4; wake++) {
            assert(config_init() == ESP_OK);
            assert(!setup_should_reopen(config_take_paired_pending(), false, false));
        }

        // Saving again arms exactly one check of the selected service.
        config_set_paired_pending(true);
        assert(setup_should_reopen(config_take_paired_pending(), true, false));
        assert(!setup_should_reopen(config_take_paired_pending(), true, false));

        // A confirmed connection needs no image or completed claim to pass.
        config_set_paired_pending(true);
        assert(!setup_should_reopen(config_take_paired_pending(), true, true));
        assert(config_init() == ESP_OK);
        assert(!setup_should_reopen(config_take_paired_pending(), true, false));
        assert(!setup_should_reopen(config_take_paired_pending(), false, false));

        char ssid[33], pass[65], url[128], token[64];
        assert(config_get_wifi(ssid, sizeof ssid, pass, sizeof pass));
        assert(!strcmp(ssid, "setup-network") && !strcmp(pass, "setup-password"));
        config_get_server_url(url, sizeof url);
        assert(!strcmp(url, "http://server.test"));
        config_get_device_token(token, sizeof token);
        assert(!strcmp(token, "saved-token"));
        assert(config_relay_configured() == (transport == 2));
    }
}

int main(void) {
    char ssid[33], pass[65], url[128], token[64];
    assert(config_init() == ESP_OK);
    uint8_t photo_key[32], read_key[32]; memset(photo_key, 0x5a, sizeof photo_key);
    assert(!config_screen_is_bluetooth());
    assert(!config_get_photo_key(read_key));
    assert(config_save_screen_mode(true, NULL) == ESP_ERR_INVALID_ARG);
    fail_commit = 1;
    assert(config_save_screen_mode(true, photo_key) == ESP_FAIL);
    fail_commit = 0;
    assert(config_save_screen_mode(true, photo_key) == ESP_OK);
    assert(config_screen_is_bluetooth());
    assert(config_get_photo_key(read_key) && !memcmp(photo_key, read_key, 32));

    config_set_server_url("http://example.test:8000");
    config_set_device_token("test-token");
    config_set_transport(0);
    config_set_mqtt("mqtt://broker.test", "test-user", "test-password");
    config_set_relay_url("https://relay.test");
    assert(config_save_wifi("test-network", "test-password") == ESP_OK);
    assert(config_save_waveform(1) == ESP_OK);
    assert(config_get_waveform(0) == 1);
    unsigned count = commits;
    assert(config_save_waveform(1) == ESP_OK && commits == count);
    assert(config_save_waveform(3) == ESP_ERR_INVALID_ARG && commits == count);
    assert(config_get_waveform(0) == 1);
    fail_commit = 1;
    assert(config_save_waveform(2) == ESP_FAIL);
    fail_commit = 0;
    assert(config_save_waveform(0) == ESP_OK);
    assert(config_clear_wifi() == ESP_OK);
    assert(!config_get_wifi(ssid, sizeof ssid, pass, sizeof pass));
    assert(config_screen_is_bluetooth()); // clearing Wi-Fi preserves manual mode
    assert(config_get_photo_key(read_key) && !memcmp(photo_key, read_key, 32));
    assert(config_take_ble_recovery());
    assert(!config_take_ble_recovery());
    assert(config_get_waveform(2) == 0);
    config_get_server_url(url, sizeof url); assert(!strcmp(url, "http://example.test:8000"));
    config_get_device_token(token, sizeof token); assert(!strcmp(token, "test-token"));
    assert(config_get_transport(1) == 0);
    config_get_relay_url(url, sizeof url); assert(!strcmp(url, "https://relay.test"));
    assert(config_save_wifi("open-network", "") == ESP_OK);
    assert(config_get_wifi(ssid, sizeof ssid, pass, sizeof pass));
    assert(!strcmp(ssid, "open-network") && !pass[0]);
    config_get_device_token(token, sizeof token); assert(!strcmp(token, "test-token"));
    assert(config_clear_wifi() == ESP_OK);
    config_set_wifi("portal-network", "portal-password");
    assert(config_get_wifi(ssid, sizeof ssid, pass, sizeof pass));
    assert(!strcmp(ssid, "portal-network"));
    assert(!config_take_ble_recovery());
    assert(config_save_screen_mode(false, NULL) == ESP_OK);
    assert(!config_screen_is_bluetooth() && !config_get_photo_key(read_key));
    assert(config_save_screen_mode(true, photo_key) == ESP_OK);
    assert(config_factory_reset() == ESP_OK);
    assert(!config_screen_is_bluetooth() && !config_get_photo_key(read_key));
    assert(!config_get_wifi(ssid, sizeof ssid, pass, sizeof pass));
    config_get_device_token(token, sizeof token); assert(!token[0]);
    assert(config_get_waveform(2) == 2);
    config_set_wifi("secured", "original-password");
    config_set_wifi("secured", NULL);
    assert(config_get_wifi(ssid, sizeof ssid, pass, sizeof pass) && !strcmp(pass, "original-password"));
    config_set_wifi("open", NULL);
    assert(config_get_wifi(ssid, sizeof ssid, pass, sizeof pass) && !strcmp(ssid, "open") && !pass[0]);
    config_set_wifi("another-open", "");
    assert(config_get_wifi(ssid, sizeof ssid, pass, sizeof pass) && !pass[0]);
    uint8_t pairing_key[32]; memset(pairing_key, 7, sizeof pairing_key);
    fail_commit=1;
    assert(config_set_relay_priv(pairing_key)==ESP_FAIL);
    fail_commit=0;
    assert(config_set_relay_priv(pairing_key)==ESP_OK);
    assert(config_get_relay_priv(read_key) && !memcmp(pairing_key, read_key, sizeof read_key));
    assert(!config_lowbatt_screen_pending());
    unsigned before = commits;
    assert(config_set_lowbatt_screen(false) == ESP_OK && commits == before);
    config_set_etag("frame-a");
    config_set_frame_url("http://frame.test/a");
    config_set_relay_etag("frame-b");
    config_set_device_token("preserved-token");
    assert(config_set_lowbatt_screen(true) == ESP_OK);
    before = commits;
    assert(config_set_lowbatt_screen(true) == ESP_OK && commits == before);
    assert(config_init() == ESP_OK && config_lowbatt_screen_pending());
    config_clear_frame_ref();
    config_get_etag(url, sizeof url); assert(!url[0]);
    config_get_frame_url(url, sizeof url); assert(!url[0]);
    config_get_relay_etag(url, sizeof url); assert(!url[0]);
    config_get_device_token(token, sizeof token); assert(!strcmp(token, "preserved-token"));
    assert(config_set_lowbatt_screen(false) == ESP_OK && !config_lowbatt_screen_pending());
    fail_commit = 1;
    assert(config_set_lowbatt_screen(true) == ESP_FAIL);
    fail_commit = 0;
    assert(config_set_lowbatt_screen(false) == ESP_OK);
    puts("battery screen store: persistence, unchanged writes and frame reference clearing passed");
    test_initial_connection_recovery();
    puts("setup recovery: initial failures, later outages and saved settings passed");
    puts("maintenance store: persistence failures, resets and server preservation passed");
    return 0;
}
