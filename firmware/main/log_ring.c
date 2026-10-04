// SPDX-License-Identifier: AGPL-3.0-or-later
// Modified 2026-10-01: redact credential-bearing URLs and diagnostic fields.
/*
 * log_ring.c: ring, redaction and upload batch for device log capture. Pure C;
 * see log_ring.h.
 */
#include "log_ring.h"

#include <stdio.h>
#include <string.h>

/* ---- ring ---- */

void lr_ring_reset(lr_ring_t *r, uint32_t cap, uint32_t total)
{
    r->total = total;
    r->len = 0;
    r->head = 0;
    r->cap = cap;
}

bool lr_ring_sane(const lr_ring_t *r, uint32_t cap)
{
    if (r->cap != cap || r->len > cap) return false;
    if (cap == 0) return r->len == 0 && r->head == 0;
    if (r->head >= cap) return false;
    /* Until it first wraps, a ring's head is its length. */
    return r->len == cap || r->head == r->len;
}

void lr_ring_append(lr_ring_t *r, char *data, const char *src, size_t n)
{
    r->total += (uint32_t)n;
    if (r->cap == 0 || n == 0) return;
    if (n >= r->cap) {
        /* Only the newest cap bytes survive; lay them out from index 0. */
        memcpy(data, src + (n - r->cap), r->cap);
        r->head = 0;
        r->len = r->cap;
        return;
    }
    size_t first = r->cap - r->head;
    if (first > n) first = n;
    memcpy(data + r->head, src, first);
    memcpy(data, src + first, n - first);
    r->head = (uint32_t)((r->head + n) % r->cap);
    r->len = (r->len + n > r->cap) ? r->cap : (uint32_t)(r->len + n);
}

size_t lr_ring_read(const lr_ring_t *r, const char *data,
                    uint32_t from, uint32_t to,
                    char *out, size_t out_cap, uint32_t *from_out)
{
    uint32_t oldest = lr_ring_oldest(r);
    if (lr_before(from, oldest)) from = oldest;
    if (lr_before(r->total, to)) to = r->total;
    if (from_out) *from_out = from;
    if (!lr_before(from, to) || out_cap == 0) return 0;

    size_t n = to - from;
    if (n > out_cap) n = out_cap;
    /* from is 1..len bytes behind the head. */
    uint32_t back = r->total - from;
    uint32_t idx = (r->head + r->cap - back) % r->cap;
    size_t first = r->cap - idx;
    if (first > n) first = n;
    memcpy(out, data + idx, first);
    memcpy(out + first, data, n - first);
    return n;
}

/* ---- retained state ---- */

bool lr_rtc_begin_boot(lr_rtc_hdr_t *h, uint32_t cap)
{
    bool valid = h->magic == LR_RTC_MAGIC &&
                 h->magic_inv == (uint32_t)~LR_RTC_MAGIC &&
                 lr_ring_sane(&h->ring, cap) &&
                 !lr_before(h->ring.total, h->uploaded);
    if (!valid) {
        h->magic = LR_RTC_MAGIC;
        h->magic_inv = (uint32_t)~LR_RTC_MAGIC;
        h->boot = 0;
        h->uploaded = 0;
        lr_ring_reset(&h->ring, cap, 0);
    }
    h->boot++;
    return valid;
}

uint32_t lr_rtc_unsent_from(const lr_rtc_hdr_t *h)
{
    uint32_t oldest = lr_ring_oldest(&h->ring);
    return lr_before(h->uploaded, oldest) ? oldest : h->uploaded;
}

void lr_rtc_mark_uploaded(lr_rtc_hdr_t *h, uint32_t upto)
{
    if (lr_before(upto, h->uploaded)) return;
    if (lr_before(h->ring.total, upto)) upto = h->ring.total;
    h->uploaded = upto;
}

/* ---- lines ---- */

typedef struct {
    char  *out;
    size_t len;
    size_t lim;   /* bytes that may be written, excluding reserved ones */
} sink_t;

static void put(sink_t *s, char c)
{
    if (s->len < s->lim) s->out[s->len++] = c;
}

static void puts_(sink_t *s, const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) put(s, p[i]);
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* p (with n bytes left) starts with the lower-case word w, any case. */
static bool starts_ci(const char *p, size_t n, const char *w)
{
    size_t k = strlen(w);
    if (n < k) return false;
    for (size_t i = 0; i < k; i++)
        if (lower(p[i]) != w[i]) return false;
    return true;
}

/* Where a URL or a token stops: whitespace, control bytes, quotes, angle
 * brackets. Deliberately generous about what it swallows, since a query
 * string cut short would leak its tail. */
static bool is_token_end(unsigned char c)
{
    return c <= 0x20 || c == 0x7f || c == '"' || c == '\'' || c == '<' ||
           c == '>' || c == '`';
}

size_t lr_clean(const char *in, size_t n, char *out, size_t cap)
{
    if (cap == 0) return 0;
    bool nl_end = n > 0 && in[n - 1] == '\n';
    size_t end = nl_end ? n - 1 : n;
    bool keep_nl = nl_end && cap >= 2;
    sink_t s = { .out = out, .len = 0, .lim = cap - 1 };
    if (keep_nl) s.lim--;   /* the newline always fits */

    size_t i = 0;
    while (i < end) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0x1b) {
            /* ESC [ params final: the colour codes esp_log wraps lines in.
             * A sequence cut off by line truncation just ends. */
            i++;
            if (i < end && in[i] == '[') {
                i++;
                while (i < end && in[i] >= 0x20 && in[i] <= 0x3f) i++;
                if (i < end && in[i] >= 0x40 && in[i] <= 0x7e) i++;
            } else if (i < end) {
                i++;
            }
            continue;
        }
        if (c == '\r') { i++; continue; }

        size_t scheme = 0;
        const char *schemes[] = {"https://", "http://", "mqtts://", "mqtt://", "tesserae://"};
        for (size_t k = 0; k < sizeof schemes / sizeof schemes[0]; k++)
            if (starts_ci(in + i, end - i, schemes[k])) { scheme = strlen(schemes[k]); break; }
        if (scheme) {
            puts_(&s, in + i, scheme);
            i += scheme;
            size_t authority_end = i;
            while (authority_end < end && !is_token_end((unsigned char)in[authority_end]) &&
                   in[authority_end] != '/' && in[authority_end] != '?' && in[authority_end] != '#')
                authority_end++;
            size_t host = i;
            for (size_t k = i; k < authority_end; k++) if (in[k] == '@') host = k + 1;
            if (host != i) { puts_(&s, "<redacted>@", 11); i = host; }
            // A truncated user:password URL may end before its @host part.
            bool partial_auth = false;
            if (host == i && i < authority_end && in[i] != '[') {
                for (size_t k = i; k < authority_end; k++) if (in[k] == ':') {
                    for (size_t j = k + 1; j < authority_end; j++)
                        if (in[j] < '0' || in[j] > '9') partial_auth = true;
                }
            }
            if (partial_auth) { puts_(&s, "<redacted>", 10); i = authority_end; }
            while (i < end && !is_token_end((unsigned char)in[i]) && in[i] != '?' && in[i] != '#') {
                if (starts_ci(in + i, end - i, "/v1/pair/")) {
                    puts_(&s, in + i, 9); i += 9;
                    puts_(&s, "<redacted>", 10);
                    while (i < end && !is_token_end((unsigned char)in[i]) && in[i] != '?' && in[i] != '#') i++;
                } else put(&s, in[i++]);
            }
            if (i < end && (in[i] == '?' || in[i] == '#')) {
                put(&s, in[i]); puts_(&s, "<redacted>", 10);
                while (i < end && !is_token_end((unsigned char)in[i])) i++;
            }
            continue;
        }
        if (starts_ci(in + i, end - i, "/v1/pair/")) {
            puts_(&s, in + i, 9); i += 9;
            puts_(&s, "<redacted>", 10);
            while (i < end && !is_token_end((unsigned char)in[i])) i++;
            continue;
        }
        if (starts_ci(in + i, end - i, "bearer") && i + 6 < end &&
            (in[i + 6] == ' ' || in[i + 6] == '\t')) {
            puts_(&s, in + i, 6); i += 6;
            while (i < end && (in[i] == ' ' || in[i] == '\t')) put(&s, in[i++]);
            if (i < end && !is_token_end((unsigned char)in[i])) {
                puts_(&s, "<redacted>", 10);
                while (i < end && !is_token_end((unsigned char)in[i])) i++;
            }
            continue;
        }
        const char *keys[] = {"password", "passkey", "pairing_code", "device_token",
                              "x-tesserae-token", "x-pairing-code"};
        bool field = false;
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
            size_t key_len = strlen(keys[k]);
            if (!starts_ci(in + i, end - i, keys[k])) continue;
            size_t value = i + key_len;
            if (value < end && in[value] == '"') value++;
            while (value < end && (in[value] == ' ' || in[value] == '\t')) value++;
            if (value >= end || (in[value] != ':' && in[value] != '=')) continue;
            value++;
            while (value < end && (in[value] == ' ' || in[value] == '\t')) value++;
            char quote = value < end && (in[value] == '"' || in[value] == '\'') ? in[value++] : 0;
            puts_(&s, in + i, value - i); i = value;
            puts_(&s, "<redacted>", 10);
            if (quote) {
                while (i < end && in[i] != quote) {
                    if (in[i] == '\\' && i + 1 < end) i++;
                    i++;
                }
            } else {
                while (i < end && in[i] != '\n') i++;
            }
            field = true;
            break;
        }
        if (field) continue;
        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) c = '?';
        put(&s, (char)c);
        i++;
    }
    if (keep_nl) out[s.len++] = '\n';
    out[s.len] = '\0';
    return s.len;
}

int lr_format_header(char *out, size_t cap, const char *fw, uint32_t boot,
                     const char *reset, const char *wake, uint32_t epoch)
{
    return snprintf(out, cap,
                    "# tesserae-log v1 fw=%s boot=%lu reset=%s wake=%s epoch=%lu\n",
                    fw, (unsigned long)boot, reset, wake, (unsigned long)epoch);
}

/* ---- upload batch ---- */

void lr_batch_init(lr_batch_t *b, char *buf, size_t cap)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->next = 0;
    b->started = false;
}

static bool batch_put(lr_batch_t *b, const char *p, size_t n)
{
    size_t room = b->cap - b->len;
    bool fits = n <= room;
    if (!fits) n = room;
    memcpy(b->buf + b->len, p, n);
    b->len += n;
    return fits;
}

static int marker(char *m, size_t cap, bool need_nl, unsigned long dropped)
{
    return snprintf(m, cap, "%s# ... %lu bytes dropped\n",
                    need_nl ? "\n" : "", dropped);
}

bool lr_batch_add(lr_batch_t *b, uint32_t from, const char *p, size_t n)
{
    if (b->started) {
        if (lr_before(from, b->next)) {
            uint32_t skip = b->next - from;
            if (skip >= n) return true;   /* nothing new */
            p += skip;
            n -= skip;
            from = b->next;
        } else if (from != b->next) {
            char m[LR_MARKER_MAX];
            bool need_nl = b->len > 0 && b->buf[b->len - 1] != '\n';
            int k = marker(m, sizeof m, need_nl, (unsigned long)(from - b->next));
            if (!batch_put(b, m, (size_t)k)) return false;
        }
    }
    b->started = true;
    b->next = from + (uint32_t)n;
    return batch_put(b, p, n);
}

size_t lr_batch_finish(lr_batch_t *b, size_t limit)
{
    if (b->len <= limit) return b->len;
    if (limit <= LR_MARKER_MAX) {
        b->len = 0;
        return 0;
    }
    size_t cut = b->len - (limit - LR_MARKER_MAX);   /* first byte kept */
    if (b->buf[cut - 1] != '\n') {
        /* Start on a whole line when there is one to start on. */
        const char *nl = memchr(b->buf + cut, '\n', b->len - cut);
        if (nl && (size_t)(nl - b->buf) + 1 < b->len)
            cut = (size_t)(nl - b->buf) + 1;
    }
    char m[LR_MARKER_MAX];
    int k = marker(m, sizeof m, false, (unsigned long)cut);
    memmove(b->buf + k, b->buf + cut, b->len - cut);
    memcpy(b->buf, m, (size_t)k);
    b->len = (size_t)k + (b->len - cut);
    return b->len;
}
