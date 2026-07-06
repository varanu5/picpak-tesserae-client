// test_provision_form.c — host unit test for pure portal form helpers
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "provision_form.h"

int main(void) {
    char b[128];

    // url_decode: %XX and '+'
    strcpy(b, "a+b%20c%2Fd"); provform_url_decode(b); assert(strcmp(b, "a b c/d") == 0);

    // html_escape
    char e[64]; provform_html_escape("a<b>&\"c", e, sizeof e);
    assert(strcmp(e, "a&lt;b&gt;&amp;&quot;c") == 0);

    // form_field: present, decoded; absent -> false
    const char *body = "ssid=My%20Net&pass=p%40ss&transport=rest";
    char v[64];
    assert(provform_field(body, "ssid", v, sizeof v) && strcmp(v, "My Net") == 0);
    assert(provform_field(body, "pass", v, sizeof v) && strcmp(v, "p@ss") == 0);
    assert(!provform_field(body, "missing", v, sizeof v));

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

    printf("PASS\n");
    return 0;
}
