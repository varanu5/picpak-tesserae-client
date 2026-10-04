// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "image_fetcher.h"
#include "esp_http_client.h"

struct mock_http { int unused; } client;
static int status, declared, available, received, terminal;
static bool complete, live;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    assert(!live);
    assert((cfg->crt_bundle_attach != NULL)==(strncmp(cfg->url, "https://", 8)==0));
    received=0; live=true; return &client;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len) { (void)c; (void)len; return ESP_OK; }
int esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c; return declared; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return status; }
int esp_http_client_read(esp_http_client_handle_t c, char *out, int len) {
    (void)c;
    int n=available-received;
    if (n<=0) return terminal;
    if (n>len) n=len;
    if (n>731) n=731;
    memset(out, 0xa5, n); received+=n; return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { (void)c; return complete && received==available; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; assert(live); live=false; return ESP_OK; }
esp_err_t esp_crt_bundle_attach(void *c) { (void)c; return ESP_OK; }

int main(void) {
    static uint8_t frame[30000];
    struct { int status, declared, available, terminal; bool complete, accepted; } cases[]={
        {200,30000,30000,0,true,true}, {200,0,30000,0,true,true},
        {200,40000,30000,-1,false,false}, {200,30000,30000,-1,true,false},
        {200,30000,29999,0,false,false}, {200,0,30000,0,false,false},
        {200,0,30001,0,true,false}, {200,-1,30000,0,true,false},
        {206,30000,30000,0,true,false}, {302,30000,30000,0,true,false},
        {500,30000,30000,0,true,false}, {200,29999,29999,0,true,false}
    };
    for (unsigned secure=0; secure<2; secure++) {
        for (size_t i=0; i<sizeof cases/sizeof cases[0]; i++) {
            status=cases[i].status; declared=cases[i].declared; available=cases[i].available;
            terminal=cases[i].terminal; complete=cases[i].complete;
            int result=image_fetch(secure ? "https://server.test/frame" : "http://server.test/frame", frame, sizeof frame);
            assert((result==30000)==cases[i].accepted && !live);
        }
    }
    puts("Image lengths, partial responses, read errors and complete chunked downloads passed");
}
