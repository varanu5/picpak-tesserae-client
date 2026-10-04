#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
#define ESP_EVENT_ANY_ID -1
typedef enum { MQTT_EVENT_CONNECTED, MQTT_EVENT_DATA, MQTT_EVENT_PUBLISHED,
               MQTT_EVENT_ERROR, MQTT_EVENT_DISCONNECTED } esp_mqtt_event_id_t;
typedef void *esp_mqtt_client_handle_t;
typedef struct {
    esp_mqtt_client_handle_t client;
    char *topic, *data;
    int topic_len, data_len, msg_id;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;
typedef struct {
    struct {
        struct { const char *uri; } address;
        struct { esp_err_t (*crt_bundle_attach)(void *); } verification;
    } broker;
    struct {
        const char *client_id, *username;
        struct { const char *password; } authentication;
    } credentials;
    struct { bool disable_auto_reconnect; int reconnect_timeout_ms; } network;
    struct {
        int keepalive;
        struct { const char *topic, *msg; int msg_len, qos, retain; } last_will;
    } session;
} esp_mqtt_client_config_t;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, int id,
    void (*cb)(void *, esp_event_base_t, int32_t, void *), void *arg);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *topic, int qos);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic, const char *data,
                            int len, int qos, int retain);
