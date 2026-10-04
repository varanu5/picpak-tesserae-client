// test_provision_form.c — host unit test for pure portal form helpers
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "provision_form.h"

int main(void) {
    char b[128];

    // url_decode: %XX and '+'
    strcpy(b, "a+b%20c%2Fd"); provform_url_decode(b); assert(strcmp(b, "a b c/d") == 0);

    // url_decode: malformed escapes pass through literally, never embed a NUL
    strcpy(b, "a%zzb");  provform_url_decode(b); assert(strcmp(b, "a%zzb") == 0);
    strcpy(b, "a%2");    provform_url_decode(b); assert(strcmp(b, "a%2") == 0);   // truncated escape
    strcpy(b, "a%");     provform_url_decode(b); assert(strcmp(b, "a%") == 0);
    strcpy(b, "%g1x");   provform_url_decode(b); assert(strcmp(b, "%g1x") == 0);
    strcpy(b, "a%2Gb");  provform_url_decode(b); assert(strcmp(b, "a%2Gb") == 0); // one bad digit
    strcpy(b, "%41%61"); provform_url_decode(b); assert(strcmp(b, "Aa") == 0);    // uppercase hex still works

    // html_escape
    char e[64]; provform_html_escape("a<b>&\"c", e, sizeof e);
    assert(strcmp(e, "a&lt;b&gt;&amp;&quot;c") == 0);

    // form_field: present, decoded; absent -> false
    const char *body = "ssid=My%20Net&pass=p%40ss&transport=rest&waveform=5s";
    char v[64];
    assert(provform_field(body, "ssid", v, sizeof v) && strcmp(v, "My Net") == 0);
    assert(provform_field(body, "pass", v, sizeof v) && strcmp(v, "p@ss") == 0);
    assert(provform_field(body, "transport", v, sizeof v) && strcmp(v, "rest") == 0);
    assert(provform_field(body, "waveform", v, sizeof v) && strcmp(v, "5s") == 0);
    assert(!provform_field(body, "missing", v, sizeof v));

    char ssid[33], pass[65];
    assert(provform_field("ssid=abcdefghijklmnopqrstuvwx%26%26%26%26", "ssid", ssid, sizeof ssid));
    assert(!strcmp(ssid, "abcdefghijklmnopqrstuvwx&&&&"));
    char encoded[200] = "pass=";
    for (int i=0; i<64; i++) strcat(encoded, "%21");
    assert(provform_field(encoded, "pass", pass, sizeof pass) && strlen(pass)==64);
    for (int i=0; i<64; i++) assert(pass[i]=='!');
    assert(provform_field("ssid=Caf%C3%A9+%26+home", "ssid", ssid, sizeof ssid));
    assert(!strcmp(ssid, "Caf\xc3\xa9 & home"));
    assert(provform_parse_field("ssid=12345", "ssid", ssid, 5)==PROVFORM_FIELD_INVALID);
    assert(!ssid[0]);
    const char *bad_fields[]={"ssid=x%00y", "ssid=x%2", "ssid=x%zz", "ssid=x%"};
    for (size_t i=0; i<sizeof bad_fields/sizeof bad_fields[0]; i++) {
        assert(provform_parse_field(bad_fields[i], "ssid", ssid, sizeof ssid)==PROVFORM_FIELD_INVALID);
        assert(!ssid[0]);
    }
    assert(provform_parse_field("pass=", "pass", pass, sizeof pass)==PROVFORM_FIELD_OK && !pass[0]);
    assert(provform_parse_field("ssid=x", "pass", pass, sizeof pass)==PROVFORM_FIELD_MISSING);

    // normalize_server_url
    char u[96];
    strcpy(u, "tesserae.local:8765");
    assert(provform_normalize_server_url(u, sizeof u) == PROVFORM_URL_OK
           && strcmp(u, "http://tesserae.local:8765") == 0);
    strcpy(u, "https://x:8765");
    assert(provform_normalize_server_url(u, sizeof u) == PROVFORM_URL_OK
           && strcmp(u, "https://x:8765") == 0);
    strcpy(u, "ftp://x");
    assert(provform_normalize_server_url(u, sizeof u) == PROVFORM_URL_BADSCHEME);
    strcpy(u, "");
    assert(provform_normalize_server_url(u, sizeof u) == PROVFORM_URL_EMPTY);

    // normalize_server_url: paste hygiene — trim whitespace, strip trailing '/'
    // (a kept trailing slash yields "//api/v1/..." URLs downstream)
    strcpy(u, "http://x:8765/");
    assert(provform_normalize_server_url(u, sizeof u) == PROVFORM_URL_OK
           && strcmp(u, "http://x:8765") == 0);
    strcpy(u, "http://x:8765///");                       // multiple slashes
    assert(provform_normalize_server_url(u, sizeof u) == PROVFORM_URL_OK
           && strcmp(u, "http://x:8765") == 0);
    strcpy(u, "  http://x:8765 \t");                     // surrounding whitespace
    assert(provform_normalize_server_url(u, sizeof u) == PROVFORM_URL_OK
           && strcmp(u, "http://x:8765") == 0);
    strcpy(u, "tesserae.local:8765/ ");                  // bare host + both defects
    assert(provform_normalize_server_url(u, sizeof u) == PROVFORM_URL_OK
           && strcmp(u, "http://tesserae.local:8765") == 0);
    strcpy(u, "http://x/base/");                         // path kept, slash stripped
    assert(provform_normalize_server_url(u, sizeof u) == PROVFORM_URL_OK
           && strcmp(u, "http://x/base") == 0);
    strcpy(u, "   ");                                    // whitespace-only == empty
    assert(provform_normalize_server_url(u, sizeof u) == PROVFORM_URL_EMPTY);

    // device_id validation: ^[a-z][a-z0-9_-]{1,31}$
    assert(provform_device_id_valid("picpak-1"));
    assert(provform_device_id_valid("picpak-abc123_x"));
    assert(provform_device_id_valid("ab"));                 // min length 2
    assert(!provform_device_id_valid(""));                  // empty -> invalid (auto-derive)
    assert(!provform_device_id_valid("a"));                 // too short
    assert(!provform_device_id_valid("1picpak"));           // must start with a letter
    assert(!provform_device_id_valid("Picpak"));            // no uppercase
    assert(!provform_device_id_valid("pic pak"));           // no spaces
    assert(!provform_device_id_valid("pic.pak"));           // no dots
    assert(!provform_device_id_valid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")); // 33 chars > 32

    // Relay URL reuses the server-URL normalizer: bare host gets http://, an
    // https URL passes through (trailing slash trimmed), empty rejected.
    char ru[192];
    strcpy(ru, "relay.tesserae.ink");
    assert(provform_normalize_server_url(ru, sizeof ru) == PROVFORM_URL_OK
           && strcmp(ru, "http://relay.tesserae.ink") == 0);
    strcpy(ru, "https://relay.tesserae.ink/");
    assert(provform_normalize_server_url(ru, sizeof ru) == PROVFORM_URL_OK
           && strcmp(ru, "https://relay.tesserae.ink") == 0);
    strcpy(ru, "");
    assert(provform_normalize_server_url(ru, sizeof ru) == PROVFORM_URL_EMPTY);

    printf("PASS\n");
    return 0;
}
