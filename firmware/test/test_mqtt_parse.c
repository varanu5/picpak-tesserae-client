// test_mqtt_parse.c — host unit test for pure MQTT payload/URI helpers
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "mqtt_parse.h"

int main(void) {
    char url[256];
    int32_t v;

    // --- mqtt_extract_url: bare URL ---
    const char *bare = "http://192.168.1.50:8765/renders/abc123.bin";
    assert(mqtt_extract_url(bare, strlen(bare), url, sizeof url));
    assert(strcmp(url, "http://192.168.1.50:8765/renders/abc123.bin") == 0);

    // bare URL with trailing whitespace/newline is trimmed
    const char *bare_nl = "https://host/f.bin \r\n";
    assert(mqtt_extract_url(bare_nl, strlen(bare_nl), url, sizeof url));
    assert(strcmp(url, "https://host/f.bin") == 0);

    // --- mqtt_extract_url: JSON payload ---
    const char *js = "{\"url\": \"http://h:1/x.bin\", \"format\": \"bin\"}";
    assert(mqtt_extract_url(js, strlen(js), url, sizeof url));
    assert(strcmp(url, "http://h:1/x.bin") == 0);

    // JSON with whitespace around the colon
    const char *js2 = "{ \"url\"\t : \t\"http://h/y.bin\" }";
    assert(mqtt_extract_url(js2, strlen(js2), url, sizeof url));
    assert(strcmp(url, "http://h/y.bin") == 0);

    // --- mqtt_extract_url: rejects ---
    assert(!mqtt_extract_url("", 0, url, sizeof url));                      // empty
    assert(!mqtt_extract_url("hello", 5, url, sizeof url));                 // not a URL
    assert(!mqtt_extract_url("{\"foo\":\"bar\"}", 13, url, sizeof url));    // no url key
    assert(!mqtt_extract_url("{\"url\": 42}", 11, url, sizeof url));        // url not a string
    assert(!mqtt_extract_url("{\"url\": \"\"}", 11, url, sizeof url));      // empty url value
    const char *unterm = "{\"url\": \"http://h/x";                          // no closing quote
    assert(!mqtt_extract_url(unterm, strlen(unterm), url, sizeof url));

    // payload is NOT NUL-terminated: only `len` bytes may be read
    char raw[8] = {'h','t','t','p',':','/','/','h'};
    assert(mqtt_extract_url(raw, 8, url, sizeof url));
    assert(strcmp(url, "http://h") == 0);

    // --- mqtt_extract_int ---
    const char *cfg = "{\"sleep_interval_s\": 900, \"other\": 1}";
    assert(mqtt_extract_int(cfg, strlen(cfg), "sleep_interval_s", &v) && v == 900);
    const char *neg = "{\"x\":-42}";
    assert(mqtt_extract_int(neg, strlen(neg), "x", &v) && v == -42);
    assert(!mqtt_extract_int(cfg, strlen(cfg), "missing", &v));             // absent key
    const char *notnum = "{\"sleep_interval_s\": \"soon\"}";
    assert(!mqtt_extract_int(notnum, strlen(notnum), "sleep_interval_s", &v));

    // --- mqtt_normalize_uri ---
    char uri[160];
    strcpy(uri, "192.168.1.50:1883");
    mqtt_normalize_uri(uri, sizeof uri);
    assert(strcmp(uri, "mqtt://192.168.1.50:1883") == 0);
    strcpy(uri, "mqtt://h:1883");  mqtt_normalize_uri(uri, sizeof uri);
    assert(strcmp(uri, "mqtt://h:1883") == 0);      // already schemed: unchanged
    strcpy(uri, "mqtts://h");      mqtt_normalize_uri(uri, sizeof uri);
    assert(strcmp(uri, "mqtts://h") == 0);
    strcpy(uri, "ws://h");         mqtt_normalize_uri(uri, sizeof uri);
    assert(strcmp(uri, "ws://h") == 0);
    strcpy(uri, "wss://h");        mqtt_normalize_uri(uri, sizeof uri);
    assert(strcmp(uri, "wss://h") == 0);
    strcpy(uri, "");               mqtt_normalize_uri(uri, sizeof uri);
    assert(uri[0] == '\0');                          // empty stays empty
    // no room to grow: left unchanged (esp-mqtt will report the error)
    char tiny[10]; strcpy(tiny, "h:1883");
    mqtt_normalize_uri(tiny, sizeof tiny);
    assert(strcmp(tiny, "h:1883") == 0);

    printf("PASS\n");
    return 0;
}
