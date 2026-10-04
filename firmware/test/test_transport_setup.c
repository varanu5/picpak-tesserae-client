// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config_store.h"
#include "esp_http_client.h"
#include "setup_check.h"
#include "connection_retry.h"
#include "heartbeat.h"
#include "image_fetcher.h"
#include "framebuf.h"
#include "wake_align.h"

static void check_retry_recovery(bool connected) {
    connection_retry_t retry = {.failures = 2};
    assert(connection_retry_sleep(&retry, connected, 60, 60) == (connected ? 60 : 900));
    assert(retry.failures == (connected ? 0 : 3));
}

#ifdef TEST_REST
#include "rest_handler.c"
#endif

typedef struct {
    const char *path;
    esp_http_client_method_t method;
    int status;
    const char *body;
    int bytes, declared_length;
    const char *etag;
    bool fail, close, stale_status;
    const char *location;
} response_t;
static response_t replies[16];
static size_t reply_count, reply_index;
struct mock_http {
    esp_http_client_config_t cfg;
    response_t reply;
    char url[512], headers[6][160];
    const char *post;
    bool live, complete, redirected;
};
static const char *header_names[] = {"Authorization", "X-Tesserae-Token", "X-Pairing-Code",
                                   "If-None-Match", "Content-Type", "Content-Length"};
static unsigned clients_created, external_fetches;
static char server_url[160] = "http://server.test";
static char saved_etag[80];
static char last_post[512];
static unsigned status_posts;
static uint32_t configured_sleep;
static struct mock_http client;
static char token[80], pairing[16], device[64] = "panel";

#ifndef TEST_MQTT
static void expect(const char *path, esp_http_client_method_t method, int status, const char *body) {
    assert(reply_count < sizeof replies / sizeof replies[0]);
    replies[reply_count++] = (response_t){.path=path, .method=method, .status=status, .body=body};
}
static void requests_done(void) {
    assert(reply_index == reply_count);
    assert(!client.live);
    reply_count = reply_index = 0;
}
#endif
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    assert(!client.live);
    client = (struct mock_http){.cfg = *cfg, .live = true};
    clients_created++;
#if defined(TEST_REST) || defined(TEST_RELAY)
    assert((cfg->crt_bundle_attach != NULL) == (strncmp(cfg->url, "https://", 8) == 0));
#endif
    esp_http_client_set_url(&client, cfg->url);
    return &client;
}
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url) {
    strlcpy(c->url, url, sizeof c->url); c->cfg.url = c->url; return ESP_OK;
}
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t c, void *data) {
    c->cfg.user_data = data; return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int ms) {
    c->cfg.timeout_ms = ms; return ESP_OK;
}
esp_err_t esp_http_client_reset_redirect_counter(esp_http_client_handle_t c) {
    (void)c; return ESP_OK;
}
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c) {
    assert(c->reply.location); c->redirected = true;
    return esp_http_client_set_url(c, c->reply.location);
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value) {
    for (size_t i=0; i<6; i++) if (strcmp(key, header_names[i])==0) {
        if (!value && !c->headers[i][0]) return ESP_ERR_NOT_FOUND;
        strlcpy(c->headers[i], value ? value : "", sizeof c->headers[i]);
    }
    return ESP_OK;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t method) {
    c->cfg.method = method; return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *body, int len) {
    assert((body && len > 0) || (!body && len == 0)); c->post = body;
    return body ? ESP_OK : esp_http_client_set_header(c, "Content-Type", NULL);
}
static void next_request(esp_http_client_handle_t c) {
    assert(c->live && reply_index < reply_count);
    c->reply = replies[reply_index++];
    c->complete = c->reply.status > 0 && !c->reply.fail;
    assert(strstr(c->cfg.url, c->reply.path));
    assert(c->cfg.method == c->reply.method);
    if (c->post) strlcpy(last_post, c->post, sizeof last_post);
    if (c->cfg.method == HTTP_METHOD_POST && strstr(c->url, "/status")) status_posts++;
#ifdef TEST_RELAY
    if (strstr(c->url, "/v1/i/")) assert(strcmp(c->headers[0], "Bearer relay-token") == 0);
    else assert(!c->headers[0][0]);
    if (c->cfg.method == HTTP_METHOD_GET)
        assert(!c->post && !c->headers[4][0] && !c->headers[5][0]);
    else assert(c->post && !c->headers[3][0]);
#endif
#ifdef TEST_REST
    if (strstr(c->url, "/image")) {
        for (size_t i=0; i<6; i++) assert(!c->headers[i][0]);
        assert(!c->post && c->cfg.timeout_ms == 20000);
    } else if (strstr(c->url, "/frame")) {
        assert(!c->post && !c->headers[2][0] && !c->headers[4][0] && !c->headers[5][0]);
        assert(c->headers[0][0] && c->headers[1][0]);
        if (strstr(c->url, "button=refresh")) assert(!c->headers[3][0]);
        else assert(strcmp(c->headers[3], "old-etag") == 0);
    } else if (strstr(c->url, "/log")) {
        assert(c->post && c->headers[0][0] && c->headers[1][0]);
        assert(!c->headers[2][0] && !c->headers[3][0]);
        assert(!strcmp(c->headers[4], "text/plain; charset=utf-8"));
        assert(c->cfg.timeout_ms==10000);
    } else if (strstr(c->url, "/status")) {
        assert(c->post && c->headers[0][0] && c->headers[1][0]);
        assert(!c->headers[2][0] && !c->headers[3][0]);
        assert(strcmp(c->headers[4], "application/json") == 0);
    }
#endif
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    next_request(c);
    if (c->reply.status > 0) {
        esp_http_client_event_t h = {.client=c, .event_id=HTTP_EVENT_ON_HEADER,
            .user_data=c->cfg.user_data, .header_key="ETag", .header_value=(char *)(c->reply.etag ? c->reply.etag : "new-etag")};
        c->cfg.event_handler(&h);
    }
    if (c->reply.location) {
        esp_http_client_event_t h = {.client=c, .event_id=HTTP_EVENT_ON_HEADER,
            .user_data=c->cfg.user_data, .header_key="Location", .header_value=(char *)c->reply.location};
        c->cfg.event_handler(&h);
        esp_http_client_event_t e = {.client=c, .event_id=HTTP_EVENT_REDIRECT, .user_data=c->cfg.user_data};
        c->redirected = false;
        c->cfg.event_handler(&e);
        if (c->redirected) return esp_http_client_perform(c);
    }
    if (c->reply.body) {
        int len = c->reply.bytes ? c->reply.bytes : (int)strlen(c->reply.body);
        for (int offset=0; offset<len;) {
            int n = len-offset > 1024 ? 1024 : len-offset;
            esp_http_client_event_t e = {.client=c, .event_id=HTTP_EVENT_ON_DATA,
                .user_data=c->cfg.user_data, .data=(void *)(c->reply.body+offset), .data_len=n};
            c->cfg.event_handler(&e); offset+=n;
        }
    }
    return c->reply.status <= 0 || c->reply.status == 401 || c->reply.fail ? ESP_FAIL : ESP_OK;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) {
    return c->reply.stale_status ? 200 : c->reply.status;
}
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) {
    if (c->reply.declared_length) return c->reply.declared_length;
    return c->reply.bytes ? c->reply.bytes : (c->reply.body ? (int)strlen(c->reply.body) : 0);
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return c->complete; }
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t c) { return !c->reply.close; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    assert(c->live); c->live = false; return ESP_OK;
}
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test error"; }
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
esp_err_t esp_read_mac(uint8_t *mac, int type) { (void)type; memset(mac, 1, 6); return ESP_OK; }
void config_get_server_url(char *out, size_t cap) { strlcpy(out, server_url, cap); }
void config_get_device_token(char *out, size_t cap) { strlcpy(out, token, cap); }
void config_set_device_token(const char *s) { strlcpy(token, s, sizeof token); }
void config_get_device_id(char *out, size_t cap) { strlcpy(out, device, cap); }
void config_set_device_id(const char *s) { strlcpy(device, s, sizeof device); }
void config_get_pairing_code(char *out, size_t cap) { strlcpy(out, pairing, cap); }
void config_set_pairing_code(const char *s) { strlcpy(pairing, s, sizeof pairing); }
void config_get_etag(char *out, size_t cap) { strlcpy(out, "old-etag", cap); }
void config_set_etag(const char *s) { strlcpy(saved_etag, s, sizeof saved_etag); }
uint32_t config_get_sleep_s(uint32_t fallback) { return configured_sleep ? configured_sleep : fallback; }
void config_set_sleep_s(uint32_t seconds) { configured_sleep=seconds; }
uint8_t *framebuf(void) { static uint8_t buf[30000]; return buf; }
#ifndef TEST_MQTT
int image_fetch(const char *url, uint8_t *buf, size_t size) {
    (void)url; (void)buf; (void)size; external_fetches++; return -1;
}
#endif
void heartbeat_json(char *out, size_t cap, int sleep, esp_reset_reason_t reason,
                    const char *button, uint32_t event) {
    (void)sleep; (void)reason; (void)button; (void)event; strlcpy(out, "{}", cap);
}
uint32_t wake_align_epoch(double value) { return (uint32_t)value; }
void wake_align_sync(uint32_t epoch) { (void)epoch; }
void wake_align_set_target(uint32_t epoch) { (void)epoch; }

#ifdef TEST_REST
static bool pending_report, upload_accepted;
static unsigned log_uploads, reports_acked;
bool log_capture_status(char *body, size_t cap, uint32_t *id) {
    strlcpy(body, "{\"logs\":{\"schema\":1,\"ring_bytes\":3072}}", cap);
    *id=73;
    return pending_report;
}
void log_capture_ack_report(uint32_t id) {
    assert(pending_report && id==73); reports_acked++; pending_report=false;
}
bool log_capture_upload(log_upload_fn send, void *context) {
    const char *body="# tesserae-log v1 test\nhello\n";
    log_uploads++;
    upload_accepted=send(body,strlen(body),context);
    return upload_accepted;
}
static void run_log_tests(void) {
    config_set_device_token("log-token");
    for (int secure=0;secure<2;secure++) {
        strlcpy(server_url,secure ? "https://server.test" : "http://server.test",sizeof server_url);
        unsigned before=clients_created, ack_before=reports_acked;
        pending_report=true;
        expect("/panel/frame",HTTP_METHOD_GET,304,NULL);
        expect("/panel/status",HTTP_METHOD_POST,200,"{\"next_poll_s\":123,\"logs\":{\"upload\":true}}");
        expect("/panel/log",HTTP_METHOD_POST,204,NULL);
        assert(rest_run_loop(ESP_RST_DEEPSLEEP,NULL,0)==123);
        requests_done();
        assert(upload_accepted && clients_created==before+1 && reports_acked==ack_before+1);
    }
    unsigned before=log_uploads;
    const char *ignore[]={"{\"config\":{\"logs\":{\"upload\":true}}}",
        "{\"logs\":{\"upload\":false}}", "{\"logs\":{\"upload\":1}}", "{broken"};
    for(size_t i=0;i<sizeof ignore/sizeof ignore[0];i++) {
        expect("/panel/frame",HTTP_METHOD_GET,304,NULL);
        expect("/panel/status",HTTP_METHOD_POST,200,ignore[i]);
        rest_run_loop(ESP_RST_DEEPSLEEP,NULL,0); requests_done();
    }
    assert(log_uploads==before);
    pending_report=true;
    expect("/panel/frame",HTTP_METHOD_GET,304,NULL);
    expect("/panel/status",HTTP_METHOD_POST,500,"{\"logs\":{\"upload\":true}}");
    rest_run_loop(ESP_RST_DEEPSLEEP,NULL,0); requests_done();
    assert(log_uploads==before && pending_report);
    // An upload error keeps the scheduled sleep and does not reset pairing or retry.
    const int errors[]={-1,401,403,500,413,302};
    for(size_t i=0;i<sizeof errors/sizeof errors[0];i++) {
        expect("/panel/frame",HTTP_METHOD_GET,304,NULL);
        expect("/panel/status",HTTP_METHOD_POST,200,"{\"next_poll_s\":123,\"logs\":{\"upload\":true}}");
        expect("/panel/log",HTTP_METHOD_POST,errors[i],NULL);
        if(errors[i]==302) replies[reply_count-1].location="https://other.test/log";
        if(errors[i]==-1) replies[reply_count-1].stale_status=true;
        assert(rest_run_loop(ESP_RST_DEEPSLEEP,NULL,0)==123); requests_done();
        assert(!upload_accepted && !strcmp(token,"log-token") && rest_connection_ok());
    }
    puts("REST log requests, acknowledgements, connection reuse and bounded failures passed");
}
static void expect_frame(const char *path, int bytes, bool fail) {
    static char pixels[30001];
    memset(pixels, 0xa5, sizeof pixels);
    expect(path, HTTP_METHOD_GET, 200, pixels);
    replies[reply_count-1].bytes=bytes;
    replies[reply_count-1].fail=fail;
}
static void expect_status(void) {
    expect("/panel/status", HTTP_METHOD_POST, 200, "{\"next_poll_s\":900}");
}
static void run_reuse_tests(void) {
    strlcpy(server_url, "https://server.test", sizeof server_url);
    config_set_device_token("reuse-token");
    unsigned before=clients_created;
    expect("/panel/frame", HTTP_METHOD_GET, 200, "{\"url\":\"/image?signature=test\"}");
    expect_frame("/image?signature=test", 30000, false);
    expect_status();
    rest_run_loop(ESP_RST_DEEPSLEEP, "right", 42);
    requests_done();
    assert(clients_created-before == 1 && rest_pending_frame());
    assert(!saved_etag[0]);
    for (size_t i=0; i<30000; i++) assert(rest_pending_frame()[i] == 0xa5);
    rest_frame_painted();
    assert(strcmp(saved_etag, "new-etag") == 0);

    // A refresh press clears its ETag and still reuses the connection.
    expect("/panel/frame", HTTP_METHOD_GET, 200, "{\"url\":\"/image\"}");
    expect_frame("/image", 30000, false);
    expect_status();
    rest_run_loop(ESP_RST_DEEPSLEEP, "refresh", 43);
    requests_done();
    assert(rest_pending_frame());

    // An unchanged frame still shares its connection with the heartbeat.
    before=clients_created;
    expect("/panel/frame", HTTP_METHOD_GET, 304, NULL);
    expect_status();
    rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
    requests_done();
    assert(clients_created-before == 1 && !rest_pending_frame());

    // Changing the host, port or scheme creates a new client with no credentials.
    const char *urls[] = {"https://images.test/image", "https://server.test:444/image",
                          "http://server.test/image"};
    for (size_t i=0; i<3; i++) {
        before=clients_created;
        char json[200]; snprintf(json, sizeof json, "{\"url\":\"%s\"}", urls[i]);
        expect("/panel/frame", HTTP_METHOD_GET, 200, json);
        expect_frame("/image", 30000, false);
        expect_status();
        rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
        requests_done();
        assert(clients_created-before == 3 && rest_pending_frame());
    }

    // Refuse short, oversized and interrupted bodies, even with a full buffer.
    const int sizes[]={29999,30001,30000};
    for (size_t i=0; i<3; i++) {
        expect("/panel/frame", HTTP_METHOD_GET, 200, "{\"url\":\"/image\"}");
        expect_frame("/image", sizes[i], i==2);
        expect_status();
        rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
        requests_done();
        assert(!rest_pending_frame());
    }

    // An incomplete JSON response is not accepted even if its bytes parse.
    expect("/panel/frame", HTTP_METHOD_GET, 200, "{\"url\":\"/image\"}");
    replies[reply_count-1].fail=true;
    expect_status();
    rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
    requests_done();
    assert(!rest_pending_frame());

    // A stale socket retries the image GET once, ignoring an old status code.
    before=clients_created;
    expect("/panel/frame", HTTP_METHOD_GET, 200, "{\"url\":\"/image\"}");
    expect("/image", HTTP_METHOD_GET, -1, NULL);
    replies[reply_count-1].stale_status=true;
    expect_frame("/image", 30000, false);
    expect_status();
    rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
    requests_done();
    assert(clients_created-before == 2 && rest_pending_frame());

    // A failed retry is bounded and does not prevent a fresh status request.
    expect("/panel/frame", HTTP_METHOD_GET, 200, "{\"url\":\"/image\"}");
    expect("/image", HTTP_METHOD_GET, -1, NULL);
    expect("/image", HTTP_METHOD_GET, -1, NULL);
    expect_status();
    rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
    requests_done();
    assert(!rest_pending_frame());

    // Connection: close is respected before the next request.
    before=clients_created;
    expect("/panel/frame", HTTP_METHOD_GET, 200, "{\"url\":\"/image\"}");
    replies[reply_count-1].close=true;
    expect_frame("/image", 30000, false);
    expect_status();
    rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
    requests_done();
    assert(clients_created-before == 2 && rest_pending_frame());

    // Heartbeats are not replayed after an ambiguous connection failure.
    expect("/panel/frame", HTTP_METHOD_GET, 304, NULL);
    expect("/panel/status", HTTP_METHOD_POST, -1, NULL);
    replies[reply_count-1].stale_status=true;
    rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
    requests_done();
    assert(rest_connection_ok());

    // API redirects retain their existing behaviour but the client is discarded.
    before=clients_created;
    expect("/panel/frame", HTTP_METHOD_GET, 302, NULL);
    replies[reply_count-1].location="https://server.test/redirected";
    expect("/redirected", HTTP_METHOD_GET, 204, NULL);
    expect_status();
    rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
    requests_done();
    assert(clients_created-before == 2 && rest_connection_ok());

    const char *unsafe[]={"https://other.test/frame", "http://server.test/frame",
        "https://server.test:444/frame", "https://server.test@other.test/frame"};
    for (size_t i=0; i<sizeof unsafe/sizeof unsafe[0]; i++) {
        expect("/panel/frame", HTTP_METHOD_GET, 302, NULL);
        replies[reply_count-1].location=unsafe[i];
        expect_status();
        rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
        requests_done();
        assert(!rest_pending_frame());
        expect("/panel/status", HTTP_METHOD_POST, 307, NULL);
        replies[reply_count-1].location=unsafe[i];
        resp_t response={0};
        assert(http_do(HTTP_METHOD_POST, "https://server.test/panel/status", "token", NULL,
                       NULL, "{}", &response)==307);
        requests_done();
    }

    // Images do not follow redirects or forward the device token.
    expect("/panel/frame", HTTP_METHOD_GET, 200, "{\"url\":\"/image\"}");
    expect("/image", HTTP_METHOD_GET, 302, NULL);
    replies[reply_count-1].location="https://other.test/image";
    expect_status();
    rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0);
    requests_done();
    assert(!rest_pending_frame());

    // Pairing data must not remain on subsequent frame requests.
    config_set_device_token("");
    config_set_pairing_code("123456");
    before=clients_created;
    expect("/register", HTTP_METHOD_POST, 201,
           "{\"device_token\":\"new-token\",\"device_id\":\"panel\"}");
    expect("/panel/frame", HTTP_METHOD_GET, 204, NULL);
    expect_status();
    rest_run_loop(ESP_RST_SW, NULL, 0);
    requests_done();
    assert(clients_created-before == 1 && !pairing[0]);
    assert(external_fetches == 0);
    strlcpy(server_url, "http://server.test", sizeof server_url);
    puts("REST connection reuse, request isolation and failure tests passed");
}
static void rest_cycle(int frame_status, int status_status, bool reachable) {
    config_set_device_token("old-token");
    expect("/panel/frame", HTTP_METHOD_GET, frame_status, "<html>wrong service</html>");
    expect("/panel/status", HTTP_METHOD_POST, status_status, "<html>wrong service</html>");
    assert(rest_run_loop(ESP_RST_SW, NULL, 0) == 900);
    requests_done();
    assert(rest_connection_ok() == reachable);
    check_retry_recovery(rest_connection_ok());
    assert(setup_should_reopen(true, true, rest_connection_ok()) == !reachable);
    assert(!setup_should_reopen(false, true, rest_connection_ok()));
    assert(rest_pending_frame() == NULL);
}
int main(void) {
    rest_cycle(401, 401, true);
    assert(!token[0]);
    // The next wake discovers a replacement token without opening setup.
    expect("/discover", HTTP_METHOD_POST, 200,
           "{\"registered\":true,\"device_token\":\"new-token\",\"device_id\":\"panel\"}");
    expect("/panel/frame", HTTP_METHOD_GET, 204, NULL);
    expect("/panel/status", HTTP_METHOD_POST, 200, "{\"next_poll_s\":900}");
    assert(rest_run_loop(ESP_RST_DEEPSLEEP, NULL, 0) == 900);
    requests_done();
    assert(strcmp(token, "new-token") == 0 && rest_connection_ok());
    rest_cycle(403, 403, true);
    assert(!token[0]);
    rest_cycle(401, -1, true);
    rest_cycle(-1, 401, true);
    rest_cycle(304, -1, true);
    rest_cycle(204, -1, true);
    rest_cycle(-1, -1, false);
    rest_cycle(503, 503, false);
    rest_cycle(404, 404, false);
    rest_cycle(200, 200, false);
    config_set_device_token("");
    config_set_pairing_code("123456");
    expect("/register", HTTP_METHOD_POST, 403, "{}");
    rest_run_loop(ESP_RST_SW, NULL, 0);
    requests_done();
    assert(!rest_connection_ok() && !pairing[0]);
    assert(setup_should_reopen(true, true, rest_connection_ok()));
    expect("/discover", HTTP_METHOD_POST, 200, "{\"registered\":false,\"retry_after_s\":30}");
    assert(rest_run_loop(ESP_RST_SW, NULL, 0) == 30);
    requests_done();
    assert(rest_connection_ok() && !token[0]);
    puts("REST setup and token renewal tests passed");
    run_reuse_tests();
    run_log_tests();
}
#endif

#ifdef TEST_RELAY
#include "relay.c"

static bool paired, have_private, private_save_fails;
static unsigned keypairs;
static char relay_code[40] = "pair-code";
static char relay_url[160] = "https://relay.test";
static char relay_etag[80], relay_cfg_etag[80];
static bool unseal_ok;
static unsigned unseal_calls;
// Only retained authentication state survives a simulated deep sleep.
static void relay_boot(void) {
    assert(!client.live);
    s_connection_ok = false;
    s_auth_noted_this_wake = false;
    s_frame_pending = false;
    s_have_advertised_cfg = false;
}
bool config_relay_ready(void) { return paired; }
bool config_relay_configured(void) { return paired || relay_code[0]; }
void config_get_relay_url(char *out, size_t cap) { strlcpy(out, relay_url, cap); }
void config_get_relay_code(char *out, size_t cap) { strlcpy(out, relay_code, cap); }
bool config_get_relay_priv(uint8_t priv[32]) { memset(priv, 1, 32); return have_private; }
esp_err_t config_set_relay_priv(const uint8_t priv[32]) { (void)priv; if (private_save_fails) return ESP_FAIL; have_private = true; return ESP_OK; }
void config_clear_relay(void) { paired = have_private = false; relay_code[0] = 0; }
void config_forget_relay_pairing(void) { config_clear_relay(); }
void config_set_relay_paired(const char *install, const char *dev, const char *tok, const uint8_t key[32]) {
    assert(install[0] && dev[0] && tok[0] && key[0]); paired = true;
}
void config_get_relay_install(char *out, size_t cap) { strlcpy(out, "install", cap); }
void config_get_relay_device(char *out, size_t cap) { strlcpy(out, "panel", cap); }
void config_get_relay_token(char *out, size_t cap) { strlcpy(out, "relay-token", cap); }
bool config_get_relay_key(uint8_t key[32]) { memset(key, 1, 32); return paired; }
void config_get_relay_etag(char *out, size_t cap) { strlcpy(out, relay_etag, cap); }
void config_set_relay_etag(const char *s) { strlcpy(relay_etag, s, sizeof relay_etag); }
void config_get_relay_config_etag(char *out, size_t cap) { strlcpy(out, relay_cfg_etag, cap); }
void config_set_relay_config_etag(const char *s) { strlcpy(relay_cfg_etag, s, sizeof relay_cfg_etag); }
bool wifi_get_sta_ip(char *out, size_t cap) { strlcpy(out, "192.0.2.1", cap); return true; }
int wifi_rssi(void) { return -60; }
int power_battery_mv(void) { return 4000; }
int power_battery_pct(int mv) { (void)mv; return 80; }

// Crypto is stubbed here because these tests exercise connection decisions.
void relay_public_key(uint8_t pub[32], const uint8_t priv[32]) { assert(priv[0] == 1); memset(pub, 2, 32); }
void relay_keypair(uint8_t priv[32], uint8_t pub[32]) { keypairs++; memset(priv, 1, 32); memset(pub, 2, 32); }
bool relay_b64url_encode(char *out, size_t cap, const uint8_t *in, size_t len) {
    (void)in; (void)len; strlcpy(out, "test-public-key", cap); return true;
}
bool relay_b64url_decode(uint8_t *out, size_t cap, size_t *len, const char *in) {
    assert(cap >= 32 && strcmp(in, "test-public-key") == 0);
    memset(out, 2, 32); *len = 32; return true;
}
bool relay_derive_key(uint8_t key[32], const uint8_t priv[32], const uint8_t pub[32]) {
    (void)priv; (void)pub; memset(key, 3, 32); return true;
}
bool relay_unseal(uint8_t *blob, size_t len, const uint8_t key[32], uint8_t **frame, size_t *size) {
    (void)key; unseal_calls++;
    if (!unseal_ok || len < RELAY_SEAL_OVERHEAD) return false;
    *frame=blob+RELAY_NONCE_LEN; *size=len-RELAY_SEAL_OVERHEAD; return true;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len) {
    (void)len; next_request(c);
    return c->reply.status <= 0 ? ESP_FAIL : ESP_OK;
}
int esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c; return 0; }
int esp_http_client_read(esp_http_client_handle_t c, char *out, int len) {
    (void)c; (void)out; (void)len; return 0;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }

static void expect_relay_blob(int bytes, bool fail) {
    static char blob[30029];
    memset(blob, 0xa5, sizeof blob);
    expect("/frame", HTTP_METHOD_GET, 200, blob);
    replies[reply_count-1].bytes=bytes;
    replies[reply_count-1].fail=fail;
}
static void relay_reuse_tests(void) {
    paired=true; unseal_ok=true;
    relay_boot();
    unsigned before=clients_created, posts=status_posts;
    expect("/frame", HTTP_METHOD_GET, 304, NULL);
    expect("/status", HTTP_METHOD_POST, 200, "{}");
    expect("/config", HTTP_METHOD_GET, 204, NULL);
    assert(relay_run_loop("right", 42) == 900);
    assert(client.live && !client.cfg.user_data && !relay_pending_frame());
    assert(strstr(last_post, "\"button\":\"right\"") && strstr(last_post, "\"button_event_id\":42"));
    expect("/frame", HTTP_METHOD_GET, 304, NULL);
    assert(!relay_poll_frame());
    expect_relay_blob(30028, false);
    assert(relay_poll_frame());
    assert(!relay_etag[0]);
    for (size_t i=0; i<30000; i++) assert(relay_pending_frame()[i] == 0xa5);
    relay_end_wake(); relay_end_wake(); requests_done();
    assert(clients_created-before == 1 && status_posts-posts == 1);
    relay_frame_painted();
    assert(strcmp(relay_etag, "new-etag") == 0 && !relay_pending_frame());

    // A stale socket inside the button window gets one fresh GET attempt.
    relay_boot(); before=clients_created;
    expect("/frame", HTTP_METHOD_GET, 304, NULL); assert(!relay_poll_frame());
    expect("/frame", HTTP_METHOD_GET, -1, NULL); replies[reply_count-1].stale_status=true;
    expect_relay_blob(30028, false); assert(relay_poll_frame());
    relay_end_wake(); requests_done(); assert(clients_created-before == 2);

    // A second failure ends the retry without discarding pairing.
    relay_boot();
    expect("/frame", HTTP_METHOD_GET, 304, NULL); assert(!relay_poll_frame());
    expect("/frame", HTTP_METHOD_GET, -1, NULL);
    expect("/frame", HTTP_METHOD_GET, -1, NULL); assert(!relay_poll_frame());
    relay_end_wake(); requests_done(); assert(paired);

    // A server that closes each response remains usable.
    relay_boot(); before=clients_created;
    expect("/frame", HTTP_METHOD_GET, 304, NULL); replies[reply_count-1].close=true;
    expect("/status", HTTP_METHOD_POST, 200, "{}"); replies[reply_count-1].close=true;
    expect("/config", HTTP_METHOD_GET, 204, NULL);
    relay_run_loop(NULL, 0); relay_end_wake(); requests_done();
    assert(clients_created-before == 3);

    // A failed button status POST is not sent a second time.
    relay_boot(); posts=status_posts;
    expect("/frame", HTTP_METHOD_GET, 304, NULL);
    expect("/status", HTTP_METHOD_POST, -1, NULL); replies[reply_count-1].stale_status=true;
    expect("/config", HTTP_METHOD_GET, 204, NULL);
    relay_run_loop("right", 43); relay_end_wake(); requests_done();
    assert(status_posts-posts == 1 && paired);

    // Every download must finish before the authentication check runs.
    for (int i=0; i<4; i++) {
        relay_boot(); unsigned calls=unseal_calls;
        expect_relay_blob(i==1 ? 30029 : 30028, i==0);
        if (i==1) replies[reply_count-1].declared_length=30028;
        if (i==2) replies[reply_count-1].declared_length=30029;
        if (i==3) replies[reply_count-1].declared_length=-1;
        assert(!relay_poll_frame()); relay_end_wake(); requests_done();
        assert(unseal_calls==calls && paired);
    }
    // Authentication and plaintext size failures never stage a frame.
    relay_boot(); unseal_ok=false;
    expect_relay_blob(30028, false); assert(!relay_poll_frame());
    relay_end_wake(); requests_done(); assert(!relay_etag[0]); unseal_ok=true;
    relay_boot(); expect_relay_blob(30027, false); assert(!relay_poll_frame());
    relay_end_wake(); requests_done(); assert(!relay_etag[0]);

    // Frame, status and encrypted configuration can all share the connection.
    relay_boot(); before=clients_created;
    char cfg[128]={0}; const char *json="{\"sleep_interval_s\":120}";
    memcpy(cfg+RELAY_NONCE_LEN, json, strlen(json));
    expect("/frame", HTTP_METHOD_GET, 304, NULL);
    expect("/status", HTTP_METHOD_POST, 200, "{\"config_etag\":\"cfg-new\"}");
    expect("/config", HTTP_METHOD_GET, 200, cfg);
    replies[reply_count-1].bytes=(int)strlen(json)+RELAY_SEAL_OVERHEAD;
    replies[reply_count-1].etag="cfg-new";
    assert(relay_run_loop(NULL, 0)==120);
    relay_end_wake(); requests_done();
    assert(clients_created-before==1 && strcmp(relay_cfg_etag, "cfg-new")==0);
    relay_boot();
    expect("/frame", HTTP_METHOD_GET, 304, NULL);
    expect("/status", HTTP_METHOD_POST, 200, "{\"config_etag\":\"cfg-new\"}");
    relay_run_loop(NULL, 0); relay_end_wake(); requests_done();
    configured_sleep=0; relay_cfg_etag[0]=0;

    // Changing origin cannot reuse the previous socket.
    relay_boot(); before=clients_created;
    expect("/frame", HTTP_METHOD_GET, 304, NULL); relay_poll_frame();
    const char *origins[]={"https://other.test", "https://other.test:444", "http://other.test"};
    for (size_t i=0; i<3; i++) {
        strlcpy(relay_url, origins[i], sizeof relay_url);
        expect("/frame", HTTP_METHOD_GET, 304, NULL); relay_poll_frame();
    }
    relay_end_wake(); requests_done(); assert(clients_created-before==4);
    strlcpy(relay_url, "https://relay.test", sizeof relay_url);

    // A sealed mailbox redirect is not followed or authenticated as a frame.
    relay_boot();
    expect("/frame", HTTP_METHOD_GET, 302, NULL);
    replies[reply_count-1].location="https://other.test/frame";
    assert(!relay_poll_frame()); relay_end_wake(); requests_done();
    // An initial frame survives unchanged polls, read failures and bad tags.
    relay_boot();
    expect_relay_blob(30028, false); replies[reply_count-1].etag="initial";
    expect("/status", HTTP_METHOD_POST, 200, "{}");
    expect("/config", HTTP_METHOD_GET, 204, NULL);
    relay_run_loop("right", 44);
    assert(relay_pending_frame() && !strcmp(s_pending_etag, "initial"));
    expect("/frame", HTTP_METHOD_GET, 304, NULL);
    assert(!relay_poll_frame() && relay_pending_frame());
    assert(!strcmp(client.headers[3], "initial"));
    expect_relay_blob(30028, false); replies[reply_count-1].etag="initial";
    assert(!relay_poll_frame() && relay_pending_frame());
    expect_relay_blob(30028, true);
    assert(!relay_poll_frame() && relay_pending_frame());
    unseal_ok=false; expect_relay_blob(30028, false);
    assert(!relay_poll_frame() && relay_pending_frame()); unseal_ok=true;
    for (size_t i=0; i<30000; i++) assert(relay_pending_frame()[i]==0xa5);
    expect_relay_blob(30028, false); replies[reply_count-1].etag="navigation";
    assert(relay_poll_frame() && !strcmp(s_pending_etag, "navigation"));
    relay_end_wake(); requests_done(); relay_frame_painted();
    assert(!strcmp(relay_etag, "navigation"));

    // JSON redirects also retain credentials only on the original origin.
    const char *redirects[]={"https://other.test/status", "http://relay.test/status",
                            "https://relay.test/v1/i/install/d/panel/status"};
    for(size_t i=0; i<3; i++) {
        char response[64];
        expect("/status", HTTP_METHOD_POST, 307, NULL);
        replies[reply_count-1].location=redirects[i];
        if(i==2) expect("/status", HTTP_METHOD_POST, 200, "{}");
        assert(relay_json("https://relay.test/v1/i/install/d/panel/status", "{}", "relay-token",
                          response, sizeof response)==(i==2 ? 200 : 307));
        relay_end_wake(); requests_done();
    }
    puts("Relay connection reuse, button polling, frame validation and recovery tests passed");
}

static void relay_cycle(int status, bool reachable) {
    relay_boot();
    expect("/frame", HTTP_METHOD_GET, status, NULL);
    expect("/status", HTTP_METHOD_POST, status, "{}");
    expect("/config", HTTP_METHOD_GET, status, NULL);
    assert(relay_run_loop(NULL, 0) == 900);
    relay_end_wake();
    requests_done();
    assert(relay_connection_ok() == reachable);
    check_retry_recovery(relay_connection_ok());
    assert(setup_should_reopen(true, true, relay_connection_ok()) == !reachable);
    assert(!setup_should_reopen(false, true, relay_connection_ok()));
}
int main(void) {
    paired = true;
    relay_cycle(401, true);
    assert(s_auth_fail_streak == 1 && !relay_pairing_revoked());
    relay_cycle(401, true);
    assert(s_auth_fail_streak == 2 && relay_pairing_revoked());
    relay_forget_revoked_pairing();
    assert(!paired && !relay_pairing_revoked());
    paired = true;
    relay_cycle(401, true);
    relay_cycle(204, true);
    assert(s_auth_fail_streak == 0);
    relay_cycle(304, true);
    relay_cycle(-1, false);
    relay_cycle(404, false);
    relay_cycle(503, false);
    assert(paired);

    paired = false;
    strlcpy(relay_code, "pair-code", sizeof relay_code);
    relay_boot();
    expect("/v1/pair", HTTP_METHOD_POST, 200, "{\"status\":\"pending\"}");
    assert(relay_pair_step() == RELAY_PAIR_WAITING);
    relay_end_wake();
    requests_done();
    assert(relay_connection_ok() && have_private && !paired);
    relay_boot();
    expect("/v1/pair/pair-code", HTTP_METHOD_GET, 200, "{\"status\":\"pending\"}");
    expect("/v1/pair", HTTP_METHOD_POST, 202, "{\"status\":\"pending\"}");
    assert(relay_pair_step() == RELAY_PAIR_WAITING);
    relay_end_wake();
    requests_done();
    assert(relay_connection_ok() && !paired);
    relay_boot();
    expect("/v1/pair/pair-code", HTTP_METHOD_GET, 200,
           "{\"status\":\"ready\",\"install_id\":\"install\",\"device_id\":\"panel\","
           "\"device_token\":\"relay-token\",\"home_pubkey\":\"test-public-key\"}");
    assert(relay_pair_step() == RELAY_PAIR_DONE);
    relay_end_wake();
    requests_done();
    assert(relay_connection_ok() && paired);

    paired = false;
    relay_boot();
    expect("/v1/pair/pair-code", HTTP_METHOD_GET, 200, "<html>wrong service</html>");
    assert(relay_pair_step() == RELAY_PAIR_ERROR);
    relay_end_wake();
    requests_done();
    assert(!relay_connection_ok());
    relay_boot();
    expect("/v1/pair/pair-code", HTTP_METHOD_GET, 404, "{}");
    assert(relay_pair_step() == RELAY_PAIR_EXPIRED);
    relay_end_wake();
    requests_done();
    assert(!relay_connection_ok() && !config_relay_configured());
    // A saved key must exist before any submission can leave the device.
    strlcpy(relay_code, "pair-code", sizeof relay_code);
    private_save_fails=true; relay_boot();
    assert(relay_pair_step()==RELAY_PAIR_ERROR && !have_private && !reply_index);
    private_save_fails=false;
    expect("/v1/pair", HTTP_METHOD_POST, -1, NULL);
    assert(relay_pair_step()==RELAY_PAIR_ERROR && have_private);
    relay_end_wake(); requests_done();
    unsigned generated=keypairs;
    // The lost request may not have reached the relay. Retry the same key.
    relay_boot();
    expect("/v1/pair/pair-code", HTTP_METHOD_GET, 200, "{\"status\":\"pending\"}");
    expect("/v1/pair", HTTP_METHOD_POST, -1, NULL);
    assert(relay_pair_step()==RELAY_PAIR_ERROR && have_private && keypairs==generated);
    relay_end_wake(); requests_done();
    // Or Home completed before the retry. Polling must accept that without a new key.
    relay_boot();
    expect("/v1/pair/pair-code", HTTP_METHOD_GET, 200,
           "{\"status\":\"ready\",\"install_id\":\"install\",\"device_id\":\"panel\","
           "\"device_token\":\"relay-token\",\"home_pubkey\":\"test-public-key\"}");
    assert(relay_pair_step()==RELAY_PAIR_DONE && keypairs==generated);
    relay_end_wake(); requests_done();
    puts("Relay setup, pending pairing and revocation tests passed");
    relay_reuse_tests();
}
#endif

#ifdef TEST_MQTT
#include "mqtt_handler.c"

static EventBits_t event_bits;
static bool broker_accepts, drop_after_connect, start_fails, empty_uri;
static int subscriptions, publications;
static const char *initial_url;
static bool update_during_download;
static char displayed_url[256];
static void (*mqtt_callback)(void *, esp_event_base_t, int32_t, void *);
void config_get_mqtt(char *uri, size_t uri_cap, char *user, size_t user_cap,
                     char *pass, size_t pass_cap) {
    strlcpy(uri, empty_uri ? "" : "mqtt://broker.test", uri_cap);
    strlcpy(user, "user", user_cap); strlcpy(pass, "password", pass_cap);
}
void config_get_frame_url(char *out, size_t cap) { strlcpy(out, displayed_url, cap); }
void config_set_frame_url(const char *url) { strlcpy(displayed_url, url, sizeof displayed_url); }
static void deliver_url(const char *url) {
    esp_mqtt_event_t e={.topic=s_topic_frame, .topic_len=(int)strlen(s_topic_frame),
                        .data=(char *)url, .data_len=(int)strlen(url)};
    mqtt_callback(NULL, NULL, MQTT_EVENT_DATA, &e);
}
int image_fetch(const char *url, uint8_t *buf, size_t size) {
    char requested[256]; strlcpy(requested, url, sizeof requested);
    external_fetches++;
    if (update_during_download) deliver_url("http://server.test/B");
    assert(!strcmp(url, requested));
    memset(buf, strstr(url, "/A") ? 'A' : 'B', size);
    return (int)size;
}
EventGroupHandle_t xEventGroupCreate(void) { event_bits = 0; return &event_bits; }
void vEventGroupDelete(EventGroupHandle_t group) { assert(group == &event_bits); }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { return *group |= bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) { return *group &= ~bits; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, int clear, int all, uint32_t ticks) {
    (void)bits; (void)clear; (void)all; (void)ticks; return *group;
}
void vTaskDelay(uint32_t ticks) { (void)ticks; }
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg) {
    assert(cfg->network.disable_auto_reconnect); return &event_bits;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, int id,
        void (*cb)(void *, esp_event_base_t, int32_t, void *), void *arg) {
    (void)c; (void)id; (void)arg; mqtt_callback = cb; return ESP_OK;
}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c) {
    if (start_fails) return ESP_FAIL;
    esp_mqtt_event_t e = {.client = c};
    mqtt_callback(NULL, NULL, broker_accepts ? MQTT_EVENT_CONNECTED : MQTT_EVENT_ERROR, &e);
    if (broker_accepts && initial_url) deliver_url(initial_url);
    if (broker_accepts && drop_after_connect) {
        mqtt_callback(NULL, NULL, MQTT_EVENT_DISCONNECTED, &e);
        mqtt_callback(NULL, NULL, MQTT_EVENT_ERROR, &e);
    }
    return ESP_OK;
}
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c) { (void)c; return ESP_OK; }
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *topic, int qos) {
    (void)c; assert(topic[0] && qos == 1); return ++subscriptions;
}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic, const char *data,
                            int len, int qos, int retain) {
    (void)c; (void)len; assert(topic[0] && data[0] && qos == 1 && retain == 1);
    publications++; return -1;
}
static void mqtt_cycle(bool reachable) {
    subscriptions = publications = 0;
    assert(mqtt_run_loop(ESP_RST_SW) == 900);
    assert(mqtt_connection_ok() == reachable);
    check_retry_recovery(mqtt_connection_ok());
    assert(setup_should_reopen(true, true, mqtt_connection_ok()) == !reachable);
    assert(!setup_should_reopen(false, true, mqtt_connection_ok()));
    assert(mqtt_pending_frame() == NULL);
    assert(subscriptions == (reachable ? 2 : 0));
    assert(publications == (reachable ? 1 : 0));
}
int main(void) {
    // Broker acceptance is enough even without a frame or a successful publish.
    broker_accepts = true;
    mqtt_cycle(true);
    drop_after_connect = true;
    mqtt_cycle(true);
    // Bad credentials or an unreachable broker never produce CONNECTED.
    broker_accepts = false;
    mqtt_cycle(false);
    start_fails = true;
    mqtt_cycle(false);
    start_fails = false;
    broker_accepts = true;
    mqtt_cycle(true);
    empty_uri = true;
    mqtt_cycle(false);
    empty_uri=false; drop_after_connect=false;
    initial_url="http://server.test/A"; update_during_download=true;
    unsigned fetched=external_fetches;
    mqtt_run_loop(ESP_RST_SW);
    assert(mqtt_pending_frame() && mqtt_pending_frame()[0]=='A');
    mqtt_frame_painted(); assert(!strcmp(displayed_url, initial_url));
    initial_url="http://server.test/B"; update_during_download=false;
    mqtt_run_loop(ESP_RST_DEEPSLEEP);
    assert(mqtt_pending_frame() && mqtt_pending_frame()[0]=='B');
    mqtt_frame_painted(); assert(!strcmp(displayed_url, initial_url));
    mqtt_run_loop(ESP_RST_DEEPSLEEP);
    assert(!mqtt_pending_frame() && external_fetches==fetched+2);
    puts("MQTT setup, frame identity and connection failure tests passed");
}
#endif
