// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <stdio.h>
#include "http_redirect.h"
#include "wifi_credentials.h"
#include "button_event.h"

int main(void) {
    const char *origin = "https://server.test";
    const char *safe[] = {"/api/frame", "frame", "../frame", "?page=2",
        "https://server.test", "https://SERVER.test/api/frame"};
    const char *unsafe[] = {"", "http://server.test/frame", "https://other.test",
        "https://server.test:444/frame", "https://server.test.evil/frame",
        "https://server.test@other.test/frame", "//other.test/frame", "\\\\other.test/frame",
        "/\\other.test/frame", " https://other.test", "/path\r\nLocation:elsewhere", "ftp:other"};
    for (size_t i=0; i<sizeof safe/sizeof safe[0]; i++) assert(http_redirect_allowed(origin, safe[i]));
    for (size_t i=0; i<sizeof unsafe/sizeof unsafe[0]; i++) assert(!http_redirect_allowed(origin, unsafe[i]));
    assert(http_redirect_allowed("http://server.test:8765", "/frame"));
    assert(!http_redirect_allowed("http://server.test", "https://server.test/frame"));

    uint8_t ssid_out[32], pass_out[64];
    char ssid[34], pass[66];
    memset(ssid, 's', 32); ssid[32]=0;
    memset(pass, 'a', 64); pass[64]=0;
    assert(wifi_credentials_copy(ssid_out, pass_out, ssid, pass));
    assert(!memcmp(ssid_out, ssid, 32) && !memcmp(pass_out, pass, 64));
    pass[63]='!'; assert(!wifi_credentials_copy(ssid_out, pass_out, ssid, pass));
    pass[63]=0; assert(wifi_credentials_copy(ssid_out, pass_out, ssid, pass));
    assert(pass_out[63]==0);
    assert(wifi_credentials_copy(ssid_out, pass_out, "open", ""));
    for (size_t i=0; i<sizeof pass_out; i++) assert(!pass_out[i]);
    ssid[32]='s'; ssid[33]=0;
    assert(!wifi_credentials_copy(ssid_out, pass_out, ssid, pass));
    assert(!wifi_credentials_valid("", "password"));
    assert(!wifi_credentials_valid("network", "short"));

    uint32_t event=0;
    assert(button_event_next(&event, 0x12abcdef)==0x12abcdef);
    assert(button_event_next(&event, 0)==0x12abcdf0);
    event=0;
    assert(button_event_next(&event, 0x67abcdef)==0x67abcdef);
    event=UINT32_MAX;
    assert(button_event_next(&event, 23)==23);
    event=0; assert(button_event_next(&event, 0)!=0);
    puts("Redirect boundaries, full width WiFi credentials and button sequences passed");
}
