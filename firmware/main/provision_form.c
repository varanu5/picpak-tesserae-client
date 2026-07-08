// provision_form.c — pure form parsing/validation (no ESP deps).
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "provision_form.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Escape &, <, >, " into HTML entities. Truncates safely if dst is too small.
void provform_html_escape(const char *src, char *dst, size_t dst_sz) {
    size_t o = 0;
    for (const char *p = src; *p; p++) {
        const char *rep;
        switch (*p) {
            case '&': rep = "&amp;";  break;
            case '<': rep = "&lt;";   break;
            case '>': rep = "&gt;";   break;
            case '"': rep = "&quot;"; break;
            default:
                if (o + 1 >= dst_sz) { dst[o] = '\0'; return; }
                dst[o++] = *p;
                continue;
        }
        size_t rl = strlen(rep);
        if (o + rl >= dst_sz) break;
        memcpy(dst + o, rep, rl);
        o += rl;
    }
    dst[o] = '\0';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URL-decode %xx and '+' in-place. Caller-owned buffer. A malformed escape
// (%zz, truncated %2) passes through literally — strtol on it would yield 0
// and embed a string-truncating NUL.
void provform_url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        int hi, lo;
        if (*p == '+') { *o++ = ' '; }
        else if (*p == '%' && p[1] && p[2] &&
                 (hi = hexval(p[1])) >= 0 && (lo = hexval(p[2])) >= 0) {
            *o++ = (char)(hi << 4 | lo);
            p += 2;
        } else *o++ = *p;
    }
    *o = '\0';
}

// Pull a named field out of x-www-form-urlencoded body into dst (decoded).
bool provform_field(const char *body, const char *key, char *dst, size_t dst_sz) {
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t len = end ? (size_t)(end - v) : strlen(v);
            if (len >= dst_sz) len = dst_sz - 1;
            memcpy(dst, v, len);
            dst[len] = '\0';
            provform_url_decode(dst);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

// Validate device id: ^[a-z][a-z0-9_-]{1,31}$ (matches the form's HTML pattern
// and the Tesserae server's device_id rules). Empty is invalid here — callers
// treat "no device id" as auto-derive-from-MAC and skip this check.
bool provform_device_id_valid(const char *id) {
    if (!id) return false;
    size_t n = strlen(id);
    if (n < 2 || n > 32) return false;
    char c0 = id[0];
    if (c0 < 'a' || c0 > 'z') return false;
    for (size_t i = 1; i < n; i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                  || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Normalize a server URL in-place: bare host:port gets http:// prepended;
// http:// and https:// pass through; any other scheme is rejected; empty rejected.
provform_url_result_t provform_normalize_server_url(char *url, size_t url_sz) {
    if (!url || url[0] == '\0') return PROVFORM_URL_EMPTY;
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0)
        return PROVFORM_URL_OK;
    if (strstr(url, "://") != NULL) return PROVFORM_URL_BADSCHEME;
    char tmp[192];
    int n = snprintf(tmp, sizeof tmp, "http://%s", url);
    if (n <= 0 || (size_t)n >= url_sz) return PROVFORM_URL_BADSCHEME;
    strncpy(url, tmp, url_sz - 1);
    url[url_sz - 1] = '\0';
    return PROVFORM_URL_OK;
}
