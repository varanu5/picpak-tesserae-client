// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "portal_view.h"
#include <string.h>
bool portal_render(const char *html, portal_value_fn value, portal_send_fn send, void *ctx) {
    const char *at=html, *start;
    while ((start=strstr(at,"{{"))) {
        if (start>at && !send(at,(size_t)(start-at),ctx)) return false;
        const char *end=strstr(start+2,"}}");
        if (!end || end-start>31) return false;
        char key[32]; size_t n=(size_t)(end-start-2);
        memcpy(key,start+2,n); key[n]=0;
        const char *v=value(key,ctx);
        if (!v || !send(v,strlen(v),ctx)) return false;
        at=end+2;
    }
    return send(at,strlen(at),ctx);
}
