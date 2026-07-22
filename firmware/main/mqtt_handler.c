// mqtt_handler.c — Tesserae MQTT transport, Mode 0 (one wake cycle).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
//
// Single-session shape (spec 2026-07-17): ONE broker connect per wake —
// unlike the reference firmware's two one-shot sessions + post-paint WiFi
// reconnect (~0.1 mAh per render wake spent solely on wall-clock-accurate
// sleep_until, which our server doesn't need: next_sleep_s is primary and the
// retained heartbeat is timestamped by the server on receipt).
#include "mqtt_handler.h"
#include "mqtt_parse.h"
#include "framebuf.h"
#include "config_store.h"
#include "defaults.h"
#include "board.h"
#include "image_fetcher.h"
#include "heartbeat.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt";

#define BIT_GOT_URL  BIT0
#define BIT_FAILED   BIT1
#define BIT_PUB_DONE BIT2

static EventGroupHandle_t s_events;
static volatile bool s_connected;
static volatile int  s_pub_msg_id = -1;

// Static so the pointers handed to esp-mqtt outlive the call.
static char s_topic_frame[96];
static char s_topic_config[96];
static char s_topic_status[96];

static char s_url[256];          // retained frame URL captured by the event handler
static bool s_frame_pending;     // framebuf() holds a validated new frame
static char s_pending_url[256];  // its URL; persisted only after a successful paint

static void apply_config_payload(const char *data, int len) {
    int32_t v;
    if (!mqtt_extract_int(data, (size_t)len, "sleep_interval_s", &v)) {
        ESP_LOGW(TAG, "config payload had no sleep_interval_s; ignoring");
        return;
    }
    if (v < SLEEP_INTERVAL_MIN_S || v > SLEEP_INTERVAL_MAX_S) {
        ESP_LOGW(TAG, "config sleep_interval_s=%ld out of bounds [%d, %d]; ignoring",
                 (long)v, SLEEP_INTERVAL_MIN_S, SLEEP_INTERVAL_MAX_S);
        return;
    }
    config_set_sleep_s((uint32_t)v);
    ESP_LOGI(TAG, "config: sleep_interval_s=%ld saved", (long)v);
}

static bool topic_eq(const char *needle, const char *t, int len) {
    return (int)strlen(needle) == len && strncmp(needle, t, len) == 0;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected; subscribing to %s + %s", s_topic_frame, s_topic_config);
        esp_mqtt_client_subscribe(e->client, s_topic_frame, 1);
        esp_mqtt_client_subscribe(e->client, s_topic_config, 1);
        break;
    case MQTT_EVENT_DATA:
        if (topic_eq(s_topic_config, e->topic, e->topic_len)) {
            apply_config_payload(e->data, e->data_len);
        } else if (topic_eq(s_topic_frame, e->topic, e->topic_len)) {
            if (mqtt_extract_url(e->data, (size_t)e->data_len, s_url, sizeof s_url)) {
                ESP_LOGI(TAG, "frame url: %s", s_url);
                xEventGroupSetBits(s_events, BIT_GOT_URL);
            } else {
                ESP_LOGW(TAG, "frame payload had no usable url");
            }
        }
        break;
    case MQTT_EVENT_PUBLISHED:
        if (e->msg_id == s_pub_msg_id) xEventGroupSetBits(s_events, BIT_PUB_DONE);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt transport error");
        xEventGroupSetBits(s_events, BIT_FAILED);
        break;
    default:
        break;
    }
}

int mqtt_run_loop(esp_reset_reason_t reset_reason) {
    int fallback = (int)config_get_sleep_s(SLEEP_INTERVAL_DEFAULT_S);

    char uri[160], user[64], pass[64];
    config_get_mqtt(uri, sizeof uri, user, sizeof user, pass, sizeof pass);
    if (!uri[0]) { ESP_LOGE(TAG, "no broker URI configured"); return fallback; }
    mqtt_normalize_uri(uri, sizeof uri);

    // Device id: the provisioned id, else the same MAC-derived fallback the
    // REST path uses at pair time (deterministic -> topics stay stable).
    char dev_id[64] = {0};
    config_get_device_id(dev_id, sizeof dev_id);
    if (!dev_id[0]) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(dev_id, sizeof dev_id, "picpak-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    snprintf(s_topic_frame,  sizeof s_topic_frame,  "tesserae/%s/frame/bin", dev_id);
    snprintf(s_topic_config, sizeof s_topic_config, "tesserae/%s/config",    dev_id);
    snprintf(s_topic_status, sizeof s_topic_status, "tesserae/%s/status",    dev_id);

    s_events = xEventGroupCreate();
    if (!s_events) return fallback;
    s_connected = false;
    s_pub_msg_id = -1;
    s_url[0] = '\0';

    // LWT: a NON-retained will. The broker delivers it to live subscribers on
    // an ungraceful disconnect (keepalive timeout, TCP drop, power loss) so
    // Tesserae learns the device went dark — but because it is NOT retained it
    // never overwrites the retained heartbeat, which is what survives across
    // sleep cycles and feeds Tesserae's discovery flow. (Our graceful
    // esp_mqtt_client_stop() below doesn't trigger the will at all.)
    static const char k_lwt[] = "{\"state\":\"offline\"}";

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = dev_id,
        // Single-session design: a failed connect fails the wake (retry is the
        // next wake), so never auto-reconnect. reconnect_timeout_ms still
        // matters with reconnect disabled: the client task's WAIT_RECONNECT
        // state polls its stop flag every reconnect_timeout_ms/2, and
        // esp_mqtt_client_stop() blocks on that poll — at the 10 s default
        // that was a measured 5 s of dead radio-on time per unreachable-broker
        // wake (esp-mqtt mqtt_client.c, MQTT_STATE_WAIT_RECONNECT).
        .network.disable_auto_reconnect = true,
        .network.reconnect_timeout_ms = 1000,
        .session.keepalive = MQTT_KEEPALIVE_S,
        .session.last_will = {
            .topic   = s_topic_status,
            .msg     = k_lwt,
            .msg_len = sizeof k_lwt - 1,
            .qos     = 1,
            .retain  = 0,
        },
    };
    if (user[0]) {
        cfg.credentials.username = user;
        cfg.credentials.authentication.password = pass;
    }

    esp_mqtt_client_handle_t cli = esp_mqtt_client_init(&cfg);
    if (!cli) { vEventGroupDelete(s_events); s_events = NULL; return fallback; }
    esp_mqtt_client_register_event(cli, ESP_EVENT_ANY_ID, on_event, NULL);
    if (esp_mqtt_client_start(cli) != ESP_OK) {
        esp_mqtt_client_destroy(cli);
        vEventGroupDelete(s_events); s_events = NULL;
        return fallback;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_URL | BIT_FAILED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(MQTT_WAIT_RETAINED_MS));

    // Settle window: teardown drops queued events, and brokers typically
    // deliver the (shorter) frame/bin payload before config — give a retained
    // config right behind it time to land in apply_config_payload.
    if (bits & BIT_GOT_URL) vTaskDelay(pdMS_TO_TICKS(MQTT_SETTLE_MS));

    if (!s_connected) {
        ESP_LOGW(TAG, "broker connect failed (%s); keeping last frame", uri);
        esp_mqtt_client_stop(cli);
        esp_mqtt_client_destroy(cli);
        vEventGroupDelete(s_events); s_events = NULL;
        return fallback;
    }

    if (bits & BIT_GOT_URL) {
        char last[256] = {0};
        config_get_frame_url(last, sizeof last);
        if (strcmp(last, s_url) == 0) {
            ESP_LOGI(TAG, "frame url unchanged; skipping download");
        } else {
            int n = image_fetch(s_url, framebuf(), EPD_FB_BYTES);
            if (n == EPD_FB_BYTES) {
                // Not painted here: main paints after wifi_stop() so the radio
                // never idles through (or brown-outs) the 13-22 s EPD refresh.
                s_frame_pending = true;
                strlcpy(s_pending_url, s_url, sizeof s_pending_url);
                ESP_LOGI(TAG, "new frame buffered; painting after radio-off");
            } else {
                ESP_LOGE(TAG, "frame size %d != %d; refusing to paint", n, EPD_FB_BYTES);
            }
        }
    } else {
        ESP_LOGI(TAG, "no retained frame message within %d ms", MQTT_WAIT_RETAINED_MS);
    }

    // Heartbeat publishes even without a frame: the retained status message is
    // what makes an unclaimed device appear in Tesserae's discovery UI, and
    // what keeps telemetry flowing while a problem is being debugged.
    uint32_t sleep_s = config_get_sleep_s(SLEEP_INTERVAL_DEFAULT_S);
    char hb[512];
    heartbeat_json(hb, sizeof hb, (int)sleep_s, reset_reason, NULL, 0);  // no button report on MQTT
    xEventGroupClearBits(s_events, BIT_FAILED);   // stale fetch-phase errors don't fail the publish wait
    s_pub_msg_id = esp_mqtt_client_publish(cli, s_topic_status, hb, 0, /*qos*/ 1, /*retain*/ 1);
    if (s_pub_msg_id >= 0) {
        EventBits_t pb = xEventGroupWaitBits(s_events, BIT_PUB_DONE | BIT_FAILED,
                                             pdFALSE, pdFALSE,
                                             pdMS_TO_TICKS(MQTT_PUBACK_WAIT_MS));
        if (pb & BIT_PUB_DONE) ESP_LOGI(TAG, "heartbeat published (retained)");
        else ESP_LOGW(TAG, "heartbeat PUBACK not seen (non-fatal)");
    } else {
        ESP_LOGW(TAG, "heartbeat publish enqueue failed (non-fatal)");
    }

    // Graceful stop BEFORE main's wifi_stop(): no LWT fired. Killing WiFi
    // under a live session would mark the device offline for the whole sleep.
    esp_mqtt_client_stop(cli);
    esp_mqtt_client_destroy(cli);
    vEventGroupDelete(s_events);
    s_events = NULL;

    return (int)sleep_s;
}

const uint8_t *mqtt_pending_frame(void) {
    return s_frame_pending ? framebuf() : NULL;
}

void mqtt_frame_painted(void) {
    s_frame_pending = false;
    if (s_pending_url[0]) config_set_frame_url(s_pending_url);
}
