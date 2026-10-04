// rest_handler.c — Tesserae REST transport (one wake cycle).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "rest_handler.h"
#include "config_store.h"
#include "defaults.h"
#include "rest_button.h"
#include "framebuf.h"
#include "heartbeat.h"
#include "log_capture.h"
#include "board.h"
#include "wake_align.h"
#include "http_redirect.h"

#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdio.h>
#include <stdlib.h>    // atoi
#include <time.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "rest";
static bool    s_frame_pending = false; // framebuf() holds a validated new frame for this wake
static bool    s_connection_ok;
static char    s_pending_etag[80];      // its ETag; persisted only after a successful paint

// Generous headroom for the (small) JSON control responses: the status reply
// carries next_poll_s + server_time + a config object that may grow server-side.
// Overflow is now flagged + logged (below) instead of silently truncating.
#define REST_RESP_MAX 4096

typedef struct {
    char    *body;          // NUL-terminated accumulator (may be NULL)
    int      len;
    int      cap;
    bool     overflow;      // set when the body outgrew cap (body is truncated)
    bool     binary;
    bool     log_upload;
    size_t   upload_len;
    bool     headers_seen;
    bool     redirected;
    bool     redirect_allowed;
    char     etag[80];
    int      retry_after;   // Retry-After response header (seconds), 0 if absent
    uint32_t server_date;   // Date response header as unix epoch, 0 if unparsed
} resp_t;

// Days since the Unix epoch for a civil date (Howard Hinnant's algorithm).
static long days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097L + (long)doe - 719468L;
}

// Parse an RFC 1123 HTTP Date ("Sun, 06 Nov 1994 08:49:37 GMT") to a Unix
// epoch. Returns 0 if it does not look like a plausible recent timestamp.
static uint32_t parse_http_date(const char *v) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *comma = strchr(v, ',');
    const char *p = comma ? comma + 1 : v;
    while (*p == ' ') p++;

    int d = 0, y = 0, hh = 0, mm = 0, ss = 0; char mon[4] = {0};
    if (sscanf(p, "%d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return 0;
    const char *mp = strstr(months, mon);
    if (!mp || y < 2020 || y > 2100) return 0;
    unsigned m = (unsigned)((mp - months) / 3) + 1u;

    long long e = (long long)days_from_civil(y, m, (unsigned)d) * 86400LL
                  + hh * 3600 + mm * 60 + ss;
    return (e > 1500000000LL) ? (uint32_t)e : 0;   // sanity: past ~2017
}

static esp_http_client_handle_t s_http;
static char s_http_origin[320];

static void http_end(void) {
    if (s_http) esp_http_client_cleanup(s_http);
    s_http = NULL;
    s_http_origin[0] = '\0';
}

// A conservative match keeps different schemes, hosts and ports separate.
static bool http_origin(const char *url, char *out, size_t cap) {
    size_t scheme = strncmp(url, "https://", 8) == 0 ? 8 :
                    strncmp(url, "http://", 7) == 0 ? 7 : 0;
    if (!scheme) return false;
    size_t authority = strcspn(url + scheme, "/?#");
    size_t len = scheme + authority;
    if (!authority || len >= cap || memchr(url + scheme, '@', authority)) return false;
    memcpy(out, url, len);
    out[len] = '\0';
    return true;
}

static esp_err_t http_ev(esp_http_client_event_t *e) {
    resp_t *r = (resp_t *)e->user_data;
    if (!r) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        r->headers_seen = true;
        if (strcasecmp(e->header_key, "ETag") == 0)
            strlcpy(r->etag, e->header_value, sizeof(r->etag));
        else if (strcasecmp(e->header_key, "Retry-After") == 0)
            r->retry_after = atoi(e->header_value);
        else if (strcasecmp(e->header_key, "Date") == 0)
            r->server_date = parse_http_date(e->header_value);
        else if (strcasecmp(e->header_key, "Location") == 0)
            r->redirect_allowed = http_redirect_allowed(s_http_origin, e->header_value);
    } else if (e->event_id == HTTP_EVENT_REDIRECT) {
        r->redirected = true;
        if (!r->binary && !r->log_upload && r->redirect_allowed) {
            r->redirect_allowed = false;
            r->len = 0;
            r->overflow = false;
            r->etag[0] = '\0';
            r->retry_after = 0;
            r->server_date = 0;
            if (r->body) r->body[0] = '\0';
            esp_http_client_set_redirection(e->client);
        } else {
            ESP_LOGW(TAG, "redirect refused, use the direct server URL");
        }
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
        int status = esp_http_client_get_status_code(e->client);
        if (status >= 300 && status < 400) return ESP_OK;
        int limit = r->cap - (r->binary ? 0 : 1);
        if (r->body && !r->overflow && e->data_len <= limit - r->len) {
            memcpy(r->body + r->len, e->data, e->data_len);
            r->len += e->data_len;
            if (!r->binary) r->body[r->len] = '\0';
        } else if (r->body) {
            r->overflow = true;
        }
    }
    return ESP_OK;
}

static int http_do(esp_http_client_method_t method, const char *url,
                   const char *bearer, const char *pairing, const char *if_none_match,
                   const char *body_json, resp_t *resp) {
    char origin[sizeof(s_http_origin)] = {0};
    bool have_origin = http_origin(url, origin, sizeof(origin));
    if (!have_origin || strcasecmp(origin, s_http_origin) != 0) http_end();
    bool reused = s_http != NULL;
    resp_t initial = *resp;
    for (int attempt = 0; attempt < 2; attempt++) {
        *resp = initial;
        if (resp->body && !resp->binary) resp->body[0] = '\0';
        int timeout = resp->log_upload ? LOG_CAPTURE_TIMEOUT_MS : (resp->binary ? 20000 : 15000);
        if (!s_http) {
            esp_http_client_config_t cfg = {
                .url = url, .timeout_ms = timeout, .event_handler = http_ev,
                .user_data = resp, .disable_auto_redirect = true,
            };
            if (strncmp(url, "https://", 8) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
            s_http = esp_http_client_init(&cfg);
            if (!s_http) return -1;
            strlcpy(s_http_origin, origin, sizeof(s_http_origin));
        }
        esp_err_t err = esp_http_client_set_user_data(s_http, resp);
        if (err == ESP_OK) err = esp_http_client_set_url(s_http, url);
        if (err == ESP_OK) err = esp_http_client_set_method(s_http, method);
        if (err == ESP_OK) err = esp_http_client_set_timeout_ms(s_http, timeout);
        if (err == ESP_OK) {
            err = esp_http_client_set_post_field(s_http, NULL, 0);
            if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
        }
        // Headers belong to one request, including requests for signed images.
        const char *headers[] = {"Authorization", "X-Tesserae-Token", "X-Pairing-Code",
                                 "If-None-Match", "Content-Type", "Content-Length"};
        for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
            if (err == ESP_OK) {
                err = esp_http_client_set_header(s_http, headers[i], NULL);
                if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
            }
        }
        char auth[160];
        if (err == ESP_OK && bearer && bearer[0]) {
            snprintf(auth, sizeof(auth), "Bearer %s", bearer);
            err = esp_http_client_set_header(s_http, "Authorization", auth);
            if (err == ESP_OK) err = esp_http_client_set_header(s_http, "X-Tesserae-Token", bearer);
        }
        if (err == ESP_OK && pairing && pairing[0])
            err = esp_http_client_set_header(s_http, "X-Pairing-Code", pairing);
        if (err == ESP_OK && if_none_match && if_none_match[0])
            err = esp_http_client_set_header(s_http, "If-None-Match", if_none_match);
        if (err == ESP_OK && body_json) {
            err = esp_http_client_set_header(s_http, "Content-Type",
                resp->log_upload ? "text/plain; charset=utf-8" : "application/json");
            if (err == ESP_OK) err = esp_http_client_set_post_field(s_http, body_json,
                resp->log_upload ? (int)resp->upload_len : (int)strlen(body_json));
        }
        if (err == ESP_OK) err = esp_http_client_reset_redirect_counter(s_http);
        if (err == ESP_OK) err = esp_http_client_perform(s_http);
        int status = esp_http_client_get_status_code(s_http);
        bool complete = err == ESP_OK && esp_http_client_is_complete_data_received(s_http);
        bool persistent = complete && esp_http_client_is_persistent_connection(s_http);
        esp_http_client_set_user_data(s_http, NULL);
        if (!persistent || resp->redirected || resp->overflow || !have_origin) http_end();
        // Retry a stale connection once for GET. Never replay a pairing POST.
        if (!complete && !resp->headers_seen && reused && attempt == 0 && method == HTTP_METHOD_GET) {
            http_end();
            ESP_LOGI(TAG, "connection closed before response, retrying GET");
            continue;
        }
        // A real 401 must still renew the token when the client reports an auth error.
        if (!complete && !(resp->headers_seen && (status == 401 || status == 403))) {
            ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(err));
            return -1;
        }
        if (resp->overflow) ESP_LOGW(TAG, "response exceeds %d bytes", resp->cap);
        if (resp->server_date && !resp->binary) wake_align_sync(resp->server_date);
        return status;
    }
    return -1;
}

typedef struct {
    const char *server, *device, *token;
} log_destination_t;

static bool upload_log(const char *body, size_t len, void *context) {
    const log_destination_t *dest = context;
    char url[256];
    int n = snprintf(url, sizeof url, "%s/api/v1/device/%s/log", dest->server, dest->device);
    if (n < 0 || (size_t)n >= sizeof url) return false;
    resp_t response = { .log_upload = true, .upload_len = len };
    int status = http_do(HTTP_METHOD_POST, url, dest->token, NULL, NULL, body, &response);
    ESP_LOGI(TAG, "POST /log -> %d", status);
    return status >= 200 && status < 300;
}

static int http_frame(const char *url) {
    resp_t r = { .body = (char *)framebuf(), .cap = EPD_FB_BYTES, .binary = true };
    int status = http_do(HTTP_METHOD_GET, url, NULL, NULL, NULL, NULL, &r);
    if (status != 200 || r.overflow || r.len != EPD_FB_BYTES) {
        http_end();
        return -1;
    }
    ESP_LOGI(TAG, "fetched %d bytes (HTTP %d)", r.len, status);
    return r.len;
}

// Resolve a (possibly relative) frame URL against the server origin — the
// /frame endpoint may return a path-only url (ported from the reference's
// resolve_url; our server returns absolute URLs today, but a proxy or a server
// change must not turn into a silent every-wake fetch failure).
static void resolve_url(const char *server, const char *u, char *out, size_t cap) {
    if (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0) {
        snprintf(out, cap, "%s", u);
        return;
    }
    char origin[160];
    snprintf(origin, sizeof(origin), "%s", server);
    char *p = strstr(origin, "://");
    p = p ? p + 3 : origin;
    char *sl = strchr(p, '/');
    if (sl) *sl = '\0';                      // drop any path on the server_url
    snprintf(out, cap, "%s%s%s", origin, (u[0] == '/') ? "" : "/", u);
}

static void apply_config_sleep(cJSON *config) {
    if (!config) return;
    cJSON *sis = cJSON_GetObjectItemCaseSensitive(config, "sleep_interval_s");
    if (cJSON_IsNumber(sis)) {
        int v = sis->valueint;
        if (v >= SLEEP_INTERVAL_MIN_S && v <= SLEEP_INTERVAL_MAX_S) config_set_sleep_s(v);
    }
}

// Get a device token + id. Returns 0 when paired; otherwise the number of
// seconds to sleep before retrying (device not yet claimed / error).
//
// Two pairing paths, chosen by whether a pairing code was provisioned:
//   * pairing code present -> POST /register with X-Pairing-Code. The server
//     validates the 6-digit code and auto-claims the device (no admin click).
//     The code is single-use, so we clear it once consumed (on success, and on
//     a 403 reject so a bad code doesn't loop forever).
//   * no pairing code       -> POST /discover (friendly flow). The admin claims
//     the device from Settings -> Devices; discover returns the token once the
//     MAC matches a registered instance.
//
// The outgoing device_id is the user's provisioned id (portal "Device id"
// field), falling back to a MAC-derived picpak-xxxxxx when none is set. The
// server's canonical device_id in the response always wins and is persisted.
static int ensure_paired(const char *server) {
    char token[80] = {0};
    config_get_device_token(token, sizeof(token));
    if (token[0]) return 0;   // already paired

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // Prefer the provisioned device_id; fall back to a MAC-derived default.
    char dev_id[64] = {0};
    config_get_device_id(dev_id, sizeof(dev_id));
    if (!dev_id[0])
        snprintf(dev_id, sizeof(dev_id), "picpak-%02x%02x%02x", mac[3], mac[4], mac[5]);

    // Pairing code (optional) selects the /register vs /discover path.
    char pair[16] = {0};
    config_get_pairing_code(pair, sizeof(pair));
    bool use_register = pair[0] != '\0';

    char body[224];
    snprintf(body, sizeof(body),
        "{\"device_id\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"kind\":\"%s\",\"panel_w\":%d,\"panel_h\":%d,\"fw_version\":\"%s\"}",
        dev_id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        DEVICE_KIND, EPD_W, EPD_H, FW_VERSION);

    char url[256];
    snprintf(url, sizeof(url), "%s/api/v1/device/%s", server,
             use_register ? "register" : "discover");
    static char rbuf[REST_RESP_MAX];
    rbuf[0] = '\0';
    resp_t r = { .body = rbuf, .cap = sizeof(rbuf) };
    int st = http_do(HTTP_METHOD_POST, url, NULL,
                     use_register ? pair : NULL, NULL, body, &r);

    // Pairing-code error paths (register only; discover never sees these codes).
    if (use_register && st == 403) {
        ESP_LOGE(TAG, "pairing code rejected (403 invalid/expired); clearing it. "
                      "Re-provision with a fresh code from Settings->Devices.");
        config_set_pairing_code("");
        return REST_PAIR_REJECT_RETRY_S;
    }
    if (st == 429) {
        // Honour the server's Retry-After when present; else our hard fallback
        // (power_deep_sleep clamps the final value to the sane sleep bounds).
        int backoff = (r.retry_after > 0) ? r.retry_after : REST_PAIR_REJECT_RETRY_S;
        ESP_LOGW(TAG, "%s rate-limited (429); backing off %ds",
                 use_register ? "register" : "discover", backoff);
        return backoff;
    }
    // Register succeeds with 201, discover with 200.
    if (st != 200 && st != 201) {
        ESP_LOGW(TAG, "%s -> %d", use_register ? "register" : "discover", st);
        return REST_DISCOVER_RETRY_S;
    }

    cJSON *j = cJSON_Parse(rbuf);
    if (!j) { ESP_LOGE(TAG, "pair response not JSON"); return REST_DISCOVER_RETRY_S; }
    int retry = REST_DISCOVER_RETRY_S;
    cJSON *tok = cJSON_GetObjectItemCaseSensitive(j, "device_token");
    cJSON *id  = cJSON_GetObjectItemCaseSensitive(j, "device_id");
    cJSON *registered = cJSON_GetObjectItemCaseSensitive(j, "registered");
    if (!r.overflow && (cJSON_IsFalse(registered) ||
        (cJSON_IsString(tok) && tok->valuestring[0]))) s_connection_ok = true;
    if (cJSON_IsString(tok) && tok->valuestring[0]) {
        config_set_device_token(tok->valuestring);
        // Server's canonical device_id wins (it lowercases / may rename).
        if (cJSON_IsString(id) && id->valuestring[0]) config_set_device_id(id->valuestring);
        if (use_register) config_set_pairing_code("");   // single-use: burn on success
        apply_config_sleep(cJSON_GetObjectItemCaseSensitive(j, "config"));
        ESP_LOGI(TAG, "paired: device_id=%s", cJSON_IsString(id) ? id->valuestring : dev_id);
        retry = 0;
    } else {
        cJSON *ra = cJSON_GetObjectItemCaseSensitive(j, "retry_after_s");
        if (cJSON_IsNumber(ra) && ra->valueint > 0) retry = ra->valueint;
        ESP_LOGW(TAG, "not claimed yet as '%s' — open Tesserae Settings->Devices and Register it. retry in %ds",
                 dev_id, retry);
    }
    cJSON_Delete(j);
    return retry;
}

static int rest_wake(esp_reset_reason_t reset_reason,
                  const char *button, uint32_t button_event_id) {
    s_connection_ok = false;
    char server[160];
    config_get_server_url(server, sizeof(server));
    if (!server[0]) { ESP_LOGE(TAG, "no server URL"); return config_get_sleep_s(SLEEP_INTERVAL_DEFAULT_S); }

    int pair_retry = ensure_paired(server);
    if (pair_retry != 0) return pair_retry;   // not paired yet; retry sooner than a full interval

    char token[80];  config_get_device_token(token, sizeof(token));
    char dev_id[64]; config_get_device_id(dev_id, sizeof(dev_id));
    if (!dev_id[0]) strlcpy(dev_id, "self", sizeof(dev_id));

    // --- GET /frame ---
    char etag[80]; config_get_etag(etag, sizeof(etag));
    char url[256];
    int un = snprintf(url, sizeof(url), "%s/api/v1/device/%s/frame", server, dev_id);
    if (un > 0 && un < (int)sizeof(url) && button && button[0]) {
        // A button wake. "refresh" (5 s hold) re-renders the current page and
        // drops If-None-Match so the re-render comes back as 200; navigation
        // buttons (a tap -> "right") keep the ETag so an unchanged frame still
        // 304s. The server dedups the press by button_event_id across /frame +
        // the /status fallback.
        if (rest_button_clears_etag(button)) etag[0] = '\0';
        rest_button_query(url, (size_t)un, sizeof(url), button, button_event_id);
        ESP_LOGI(TAG, "button %s: /frame?button=%s (event %u)",
                 button, button, (unsigned)button_event_id);
    }
    static char fbuf[REST_RESP_MAX];
    fbuf[0] = '\0';
    resp_t fr = { .body = fbuf, .cap = sizeof(fbuf) };
    int st = http_do(HTTP_METHOD_GET, url, token, NULL, etag, NULL, &fr);
    ESP_LOGI(TAG, "GET /frame -> %d", st);
    // The server dispatches the button action before the frame lookup, so any of
    // 200/304/204 proves it received the press. Only these count as acknowledged;
    // an auth/network failure keeps /status as the fallback delivery path.
    bool frame_acked = (st == 200 || st == 304 || st == 204);

    if (st == 200) {
        cJSON *j = cJSON_Parse(fbuf);
        cJSON *urlj = j ? cJSON_GetObjectItemCaseSensitive(j, "url") : NULL;
        if (cJSON_IsString(urlj) && urlj->valuestring[0]) {
            if (!fr.overflow) s_connection_ok = true;
            char fullurl[320];
            resolve_url(server, urlj->valuestring, fullurl, sizeof(fullurl));
            int n = http_frame(fullurl);
            if (n == EPD_FB_BYTES) {
                // Not painted here: main paints after wifi_stop() so the radio
                // never idles through (or brown-outs) the 13-22 s EPD refresh.
                s_frame_pending = true;
                strlcpy(s_pending_etag, fr.etag, sizeof(s_pending_etag));
                ESP_LOGI(TAG, "new frame buffered; painting after radio-off");
            } else {
                ESP_LOGE(TAG, "frame size %d != %d; refusing to paint", n, EPD_FB_BYTES);
            }
        } else {
            ESP_LOGE(TAG, "frame 200 but no url in JSON body");
        }
        if (j) cJSON_Delete(j);
    } else if (st == 304) {
        s_connection_ok = true;
        ESP_LOGI(TAG, "frame unchanged (304); skipping paint");
    } else if (st == 204) {
        s_connection_ok = true;
        ESP_LOGI(TAG, "no frame rendered yet (204)");
    } else if (st == 401 || st == 403) {
        // 403 too: the server 403s a token bound to a renamed/re-canonicalized
        // device id (reference behaviour) — without the wipe we'd retry forever.
        // Keep automatic token renewal outside the initial setup check.
        s_connection_ok = true;
        ESP_LOGW(TAG, "%d; wiping token to re-pair next wake", st);
        config_set_device_token("");
    }

    // --- POST /status ---
    uint32_t sleep_s = config_get_sleep_s(SLEEP_INTERVAL_DEFAULT_S);
    char hb[768];
    // Fallback delivery: only report the button on /status if /frame didn't
    // acknowledge it (auth/network failure before the server dispatched it). The
    // server dedups by button_event_id, so a stray double-send is harmless.
    const char *btn = (button && button[0] && !frame_acked) ? button : NULL;
    heartbeat_json(hb, sizeof(hb), (int)sleep_s, reset_reason, btn, button_event_id);
    uint32_t report_id = 0;
    bool report_sent = log_capture_status(hb, sizeof hb, &report_id);
    snprintf(url, sizeof(url), "%s/api/v1/device/%s/status", server, dev_id);
    static char sbuf[REST_RESP_MAX];
    sbuf[0] = '\0';
    resp_t sr = { .body = sbuf, .cap = sizeof(sbuf) };
    int sst = http_do(HTTP_METHOD_POST, url, token, NULL, NULL, hb, &sr);
    ESP_LOGI(TAG, "POST /status -> %d", sst);

    if (report_sent && sst >= 200 && sst < 300) log_capture_ack_report(report_id);
    bool request_logs = false;
    uint32_t next = sleep_s;
    if (sst == 200 && !sr.overflow) {
        cJSON *j = cJSON_Parse(sbuf);
        if (cJSON_IsObject(j)) {
            cJSON *logs = cJSON_GetObjectItemCaseSensitive(j, "logs");
            request_logs = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(logs, "upload"));
            apply_config_sleep(cJSON_GetObjectItemCaseSensitive(j, "config"));
            cJSON *np = cJSON_GetObjectItemCaseSensitive(j, "next_poll_s");
            if (cJSON_IsNumber(np) && np->valueint > 0) s_connection_ok = true;
            if (cJSON_IsNumber(np) && np->valueint > 0) next = (uint32_t)np->valueint;
            cJSON *clock = cJSON_GetObjectItemCaseSensitive(j, "server_time");
            if (cJSON_IsNumber(clock)) wake_align_sync(wake_align_epoch(clock->valuedouble));
            cJSON *wake = cJSON_GetObjectItemCaseSensitive(j, "wake_at");
            wake_align_set_target(cJSON_IsNumber(wake) ? wake_align_epoch(wake->valuedouble) : 0);
        }
        cJSON_Delete(j);
    } else if (sst == 401 || sst == 403) {
        // Same healing as the /frame 401/403: a token revoked between the two
        // calls would otherwise take an extra full sleep cycle to re-pair.
        s_connection_ok = true;
        ESP_LOGW(TAG, "status %d; wiping token to re-pair next wake", sst);
        config_set_device_token("");
    }
    if (request_logs) {
        log_destination_t destination = { server, dev_id, token };
        log_capture_upload(upload_log, &destination);
    }
    return (int)next;
}

int rest_run_loop(esp_reset_reason_t reset_reason,
                  const char *button, uint32_t button_event_id) {
    http_end();
    s_frame_pending = false;
    s_pending_etag[0] = '\0';
    int next = rest_wake(reset_reason, button, button_event_id);
    http_end();
    return next;
}

const uint8_t *rest_pending_frame(void) {
    return s_frame_pending ? framebuf() : NULL;
}

bool rest_connection_ok(void) { return s_connection_ok; }

void rest_frame_painted(void) {
    s_frame_pending = false;
    if (s_pending_etag[0]) config_set_etag(s_pending_etag);
}
