#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST } esp_http_client_method_t;
typedef enum { HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA, HTTP_EVENT_REDIRECT } esp_http_client_event_id_t;
typedef struct mock_http *esp_http_client_handle_t;
typedef struct {
    esp_http_client_handle_t client;
    esp_http_client_event_id_t event_id;
    void *user_data;
    char *header_key, *header_value;
    void *data;
    int data_len;
} esp_http_client_event_t;
typedef struct {
    bool disable_auto_redirect;
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms, buffer_size, buffer_size_tx;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *);
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t method);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *body, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len);
int esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *out, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);

esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t c, void *data);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int ms);
esp_err_t esp_http_client_reset_redirect_counter(esp_http_client_handle_t c);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c);
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t c);

int64_t esp_http_client_get_content_length(esp_http_client_handle_t c);
