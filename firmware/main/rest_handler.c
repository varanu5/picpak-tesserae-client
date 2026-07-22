// rest_handler.c — Tesserae REST transport (one wake cycle).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "rest_handler.h"
#include "config_store.h"
#include "defaults.h"
#include "image_fetcher.h"
#include "framebuf.h"
#include "heartbeat.h"
#include "board.h"

#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdio.h>
#include <stdlib.h>    // atoi
#include <sys/time.h>  // settimeofday
#include <time.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "rest";
static bool    s_frame_pending = false; // framebuf() holds a validated new frame for this wake
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

static esp_err_t http_ev(esp_http_client_event_t *e) {
    resp_t *r = (resp_t *)e->user_data;
    if (!r) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(e->header_key, "ETag") == 0)
            strlcpy(r->etag, e->header_value, sizeof(r->etag));
        else if (strcasecmp(e->header_key, "Retry-After") == 0)
            r->retry_after = atoi(e->header_value);
        else if (strcasecmp(e->header_key, "Date") == 0)
            r->server_date = parse_http_date(e->header_value);
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
        if (r->body && r->len + e->data_len < r->cap) {
            memcpy(r->body + r->len, e->data, e->data_len);
            r->len += e->data_len;
            r->body[r->len] = '\0';
        } else if (r->body) {
            r->overflow = true;   // response bigger than cap; body is truncated
        }
    }
    return ESP_OK;
}

static int http_do(esp_http_client_method_t method, const char *url,
                   const char *bearer, const char *pairing, const char *if_none_match,
                   const char *body_json, resp_t *resp) {
    esp_http_client_config_t cfg = {
        .url = url, .method = method, .timeout_ms = 15000,
        .event_handler = http_ev, .user_data = resp,
    };
    // CA bundle only for TLS: attaching it on plain http can mis-configure the
    // client (reference-observed ESP_ERR_NOT_SUPPORTED). Publicly-trusted certs
    // only (e.g. Let's Encrypt behind a reverse proxy); self-signed won't pass.
    if (strncmp(url, "https://", 8) == 0)
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    char auth[160];
    if (bearer && bearer[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", auth);
        esp_http_client_set_header(c, "X-Tesserae-Token", bearer);
    }
    if (pairing && pairing[0]) esp_http_client_set_header(c, "X-Pairing-Code", pairing);
    if (if_none_match && if_none_match[0]) esp_http_client_set_header(c, "If-None-Match", if_none_match);
    if (body_json) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body_json, strlen(body_json));
    }
    esp_err_t err = esp_http_client_perform(c);
    // Trust the status line even when perform() reports an error. A Bearer-token
    // API 401 arrives with no WWW-Authenticate header, which makes esp_http_client
    // auto-handling fail with ESP_ERR_NOT_SUPPORTED — yet the 401 status + body
    // were received. Gating on err==ESP_OK (the old code) masked it as -1, so a
    // revoked token surfaced as a network error and was never wiped/re-paired.
    // Only a response-less failure (no status) is a real transport error.
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (status <= 0) {
        ESP_LOGW(TAG, "%s: transport error: %s", url, esp_err_to_name(err));
        return -1;
    }
    if (resp) {
        if (resp->overflow)
            ESP_LOGW(TAG, "%s: response truncated at %d bytes", url, resp->cap);
        // The server's Date header is an authoritative LAN wall clock: persist it
        // so the C3 RTC stays accurate across sleeps without an SNTP round-trip.
        // (https/mqtts cert validity still bootstraps via NTP in main.c — TLS
        // needs a sane clock *before* this response's Date can arrive.)
        if (resp->server_date) {
            struct timeval tv = { .tv_sec = (time_t)resp->server_date, .tv_usec = 0 };
            settimeofday(&tv, NULL);
        }
    }
    return status;
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

int rest_run_loop(esp_reset_reason_t reset_reason,
                  bool button_refresh, uint32_t button_event_id) {
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
    if (button_refresh && un > 0 && un < (int)sizeof(url)) {
        // A 3 s button hold: ask the server to re-render the current page (fresh
        // data — e.g. latest weather) via ?button=refresh, and drop If-None-Match
        // so the re-render comes back as 200 (a repaint), not a 304. The server
        // dedups this press by button_event_id across /frame + the /status fallback.
        etag[0] = '\0';
        snprintf(url + un, sizeof(url) - un, "?button=refresh&button_event_id=%u",
                 (unsigned)button_event_id);
        ESP_LOGI(TAG, "button refresh: /frame?button=refresh (event %u)", (unsigned)button_event_id);
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
            char fullurl[320];
            resolve_url(server, urlj->valuestring, fullurl, sizeof(fullurl));
            int n = image_fetch(fullurl, framebuf(), EPD_FB_BYTES);
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
        ESP_LOGI(TAG, "frame unchanged (304); skipping paint");
    } else if (st == 204) {
        ESP_LOGI(TAG, "no frame rendered yet (204)");
    } else if (st == 401 || st == 403) {
        // 403 too: the server 403s a token bound to a renamed/re-canonicalized
        // device id (reference behaviour) — without the wipe we'd retry forever.
        ESP_LOGW(TAG, "%d; wiping token to re-pair next wake", st);
        config_set_device_token("");
    }

    // --- POST /status ---
    uint32_t sleep_s = config_get_sleep_s(SLEEP_INTERVAL_DEFAULT_S);
    char hb[512];
    // Fallback delivery: only report the button on /status if /frame didn't
    // acknowledge it (auth/network failure before the server dispatched it). The
    // server dedups by button_event_id, so a stray double-send is harmless.
    const char *btn = (button_refresh && !frame_acked) ? "refresh" : NULL;
    heartbeat_json(hb, sizeof(hb), (int)sleep_s, reset_reason, btn, button_event_id);
    snprintf(url, sizeof(url), "%s/api/v1/device/%s/status", server, dev_id);
    static char sbuf[REST_RESP_MAX];
    sbuf[0] = '\0';
    resp_t sr = { .body = sbuf, .cap = sizeof(sbuf) };
    int sst = http_do(HTTP_METHOD_POST, url, token, NULL, NULL, hb, &sr);
    ESP_LOGI(TAG, "POST /status -> %d", sst);

    uint32_t next = sleep_s;
    if (sst == 200) {
        cJSON *j = cJSON_Parse(sbuf);
        if (j) {
            apply_config_sleep(cJSON_GetObjectItemCaseSensitive(j, "config"));
            cJSON *np = cJSON_GetObjectItemCaseSensitive(j, "next_poll_s");
            if (cJSON_IsNumber(np) && np->valueint > 0) next = (uint32_t)np->valueint;
            cJSON_Delete(j);
        }
    } else if (sst == 401 || sst == 403) {
        // Same healing as the /frame 401/403: a token revoked between the two
        // calls would otherwise take an extra full sleep cycle to re-pair.
        ESP_LOGW(TAG, "status %d; wiping token to re-pair next wake", sst);
        config_set_device_token("");
    }
    return (int)next;
}

const uint8_t *rest_pending_frame(void) {
    return s_frame_pending ? framebuf() : NULL;
}

void rest_frame_painted(void) {
    s_frame_pending = false;
    if (s_pending_etag[0]) config_set_etag(s_pending_etag);
}
