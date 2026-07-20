// mqtt_parse.c — pure MQTT payload/URI helpers (host-testable, no ESP deps).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "mqtt_parse.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>

// Portable memmem: first occurrence of needle[0..nlen) in hay[0..hlen).
static const char *find_mem(const char *hay, size_t hlen,
                            const char *needle, size_t nlen)
{
    if (nlen == 0 || hlen < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
}

bool mqtt_extract_url(const char *data, size_t len, char *dst, size_t dst_sz)
{
    if (!data || len == 0 || !dst || dst_sz == 0) return false;

    // Bare URL case
    if (len < dst_sz - 1 && (
            (len >= 7 && strncmp(data, "http://",  7) == 0) ||
            (len >= 8 && strncmp(data, "https://", 8) == 0))) {
        memcpy(dst, data, len);
        dst[len] = '\0';
        while (len && (dst[len-1] == ' ' || dst[len-1] == '\r' ||
                       dst[len-1] == '\n' || dst[len-1] == '\t')) {
            dst[--len] = '\0';
        }
        return dst[0] != '\0';
    }

    // JSON case: find "url" : "<value>"
    const char *p = find_mem(data, len, "\"url\"", 5);
    if (!p) return false;
    p += 5;
    while (p < data + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= data + len || *p != ':') return false;
    p++;
    while (p < data + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= data + len || *p != '"') return false;
    p++;
    const char *end = memchr(p, '"', (size_t)((data + len) - p));
    if (!end) return false;

    size_t vlen = (size_t)(end - p);
    if (vlen >= dst_sz) vlen = dst_sz - 1;
    memcpy(dst, p, vlen);
    dst[vlen] = '\0';
    return vlen > 0;
}

bool mqtt_extract_int(const char *data, size_t len, const char *key, int32_t *out)
{
    if (!data || !key || !out) return false;
    char needle[48];
    int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nlen <= 0 || nlen >= (int)sizeof(needle)) return false;

    const char *p = find_mem(data, len, needle, (size_t)nlen);
    if (!p) return false;
    p += nlen;

    while (p < data + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= data + len || *p != ':') return false;
    p++;
    while (p < data + len && (*p == ' ' || *p == '\t')) p++;
    if (p >= data + len) return false;

    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    if (p >= data + len || !isdigit((unsigned char)*p)) return false;

    int32_t v = 0;
    while (p < data + len && isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = v * sign;
    return true;
}

void mqtt_normalize_uri(char *uri, size_t uri_sz)
{
    if (!uri || !uri[0]) return;
    if (strncmp(uri, "mqtt://",  7) == 0) return;
    if (strncmp(uri, "mqtts://", 8) == 0) return;
    if (strncmp(uri, "ws://",    5) == 0) return;
    if (strncmp(uri, "wss://",   6) == 0) return;

    const char prefix[] = "mqtt://";
    size_t plen = sizeof(prefix) - 1;
    size_t ulen = strlen(uri);
    if (ulen + plen + 1 > uri_sz) return;   // no room; let esp-mqtt log the error
    memmove(uri + plen, uri, ulen + 1);
    memcpy(uri, prefix, plen);
}
