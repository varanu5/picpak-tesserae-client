// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "log_ring.h"

/* ---- helpers ---- */

static size_t clean_str(const char *in, char *out, size_t cap)
{
    return lr_clean(in, strlen(in), out, cap);
}

static void append_str(lr_ring_t *r, char *data, const char *s)
{
    lr_ring_append(r, data, s, strlen(s));
}

/* Whole contents of a ring, oldest first, NUL-terminated. */
static size_t ring_dump(const lr_ring_t *r, const char *data, char *out, size_t cap)
{
    uint32_t got_from = 0;
    size_t n = lr_ring_read(r, data, lr_ring_oldest(r), r->total, out, cap - 1,
                            &got_from);
    assert(got_from == lr_ring_oldest(r));
    out[n] = '\0';
    return n;
}

/* ---- ring ---- */

static void test_ring_holds_newest_bytes_across_a_wrap(void)
{
    char data[8];
    char out[32];
    lr_ring_t r;
    lr_ring_reset(&r, sizeof data, 0);

    append_str(&r, data, "abcde");
    assert(r.total == 5 && r.len == 5 && r.head == 5);
    ring_dump(&r, data, out, sizeof out);
    assert(strcmp(out, "abcde") == 0);

    append_str(&r, data, "fghij");   /* wraps: holds "cdefghij" */
    assert(r.total == 10 && r.len == 8 && r.head == 2);
    assert(lr_ring_oldest(&r) == 2);
    ring_dump(&r, data, out, sizeof out);
    assert(strcmp(out, "cdefghij") == 0);
    assert(lr_ring_sane(&r, sizeof data));
}

static void test_ring_append_longer_than_cap_keeps_the_tail(void)
{
    char data[4];
    char out[16];
    lr_ring_t r;
    lr_ring_reset(&r, sizeof data, 100);
    append_str(&r, data, "x");
    append_str(&r, data, "0123456789");
    assert(r.total == 111);
    ring_dump(&r, data, out, sizeof out);
    assert(strcmp(out, "6789") == 0);
    assert(lr_ring_sane(&r, sizeof data));
}

static void test_ring_read_clamps_and_chunks(void)
{
    char data[8];
    char out[16];
    uint32_t from = 0;
    lr_ring_t r;
    lr_ring_reset(&r, sizeof data, 0);
    append_str(&r, data, "0123456789AB");   /* holds offsets 4..11 */

    /* Before the oldest byte: clamped up. */
    size_t n = lr_ring_read(&r, data, 1, 12, out, sizeof out, &from);
    assert(from == 4 && n == 8);
    assert(memcmp(out, "456789AB", 8) == 0);

    /* Past the newest byte: clamped down. */
    n = lr_ring_read(&r, data, 10, 50, out, sizeof out, &from);
    assert(from == 10 && n == 2 && memcmp(out, "AB", 2) == 0);

    /* Chunked by out_cap, across the wrap point. */
    n = lr_ring_read(&r, data, 6, 12, out, 3, &from);
    assert(from == 6 && n == 3 && memcmp(out, "678", 3) == 0);
    n = lr_ring_read(&r, data, 9, 12, out, 3, &from);
    assert(from == 9 && n == 3 && memcmp(out, "9AB", 3) == 0);

    /* Nothing left. */
    n = lr_ring_read(&r, data, 12, 12, out, sizeof out, &from);
    assert(n == 0);
}

static void test_ring_offsets_survive_uint32_wrap(void)
{
    char data[8];
    char out[16];
    uint32_t from = 0;
    lr_ring_t r;
    lr_ring_reset(&r, sizeof data, 0xFFFFFFFCu);
    append_str(&r, data, "abcdefgh");   /* offsets 0xFFFFFFFC .. 3 */
    assert(r.total == 4);
    assert(lr_before(0xFFFFFFFEu, 2));
    size_t n = lr_ring_read(&r, data, 0xFFFFFFFEu, 2, out, sizeof out, &from);
    assert(from == 0xFFFFFFFEu && n == 4 && memcmp(out, "cdef", 4) == 0);
}

static void test_ring_sane_rejects_garbage(void)
{
    lr_ring_t r = { .total = 5, .len = 3, .head = 3, .cap = 8 };
    assert(lr_ring_sane(&r, 8));
    assert(!lr_ring_sane(&r, 16));      /* built with a different size */
    r.head = 2;
    assert(!lr_ring_sane(&r, 8));       /* head != len before a wrap */
    r.len = 8; r.head = 9;
    assert(!lr_ring_sane(&r, 8));
    r.len = 9; r.head = 0;
    assert(!lr_ring_sane(&r, 8));
}

/* ---- retained state ---- */

static void test_rtc_power_on_garbage_is_reinitialised(void)
{
    lr_rtc_hdr_t h;
    memset(&h, 0xA5, sizeof h);
    assert(!lr_rtc_begin_boot(&h, 64));
    assert(h.magic == LR_RTC_MAGIC && h.boot == 1);
    assert(h.uploaded == 0 && h.ring.total == 0 && h.ring.len == 0);
    assert(h.ring.cap == 64);
}

static void test_rtc_state_is_kept_across_boots(void)
{
    char data[64];
    lr_rtc_hdr_t h;
    memset(&h, 0, sizeof h);
    lr_rtc_begin_boot(&h, sizeof data);
    append_str(&h.ring, data, "wake one\n");
    assert(lr_rtc_begin_boot(&h, sizeof data));
    assert(h.boot == 2 && h.ring.total == 9);

    /* A ring built for another size (firmware change) starts over. */
    assert(!lr_rtc_begin_boot(&h, 32));
    assert(h.boot == 1 && h.ring.total == 0);
}

static void test_rtc_uploaded_ahead_of_total_is_invalid(void)
{
    char data[64];
    lr_rtc_hdr_t h;
    memset(&h, 0, sizeof h);
    lr_rtc_begin_boot(&h, sizeof data);
    append_str(&h.ring, data, "abc");
    h.uploaded = 10;
    assert(!lr_rtc_begin_boot(&h, sizeof data));
    assert(h.uploaded == 0);
}

static void test_rtc_uploaded_up_to(void)
{
    char data[8];
    lr_rtc_hdr_t h;
    memset(&h, 0, sizeof h);
    lr_rtc_begin_boot(&h, sizeof data);

    append_str(&h.ring, data, "0123");
    assert(lr_rtc_unsent_from(&h) == 0);
    lr_rtc_mark_uploaded(&h, 3);
    assert(h.uploaded == 3 && lr_rtc_unsent_from(&h) == 3);

    /* Never backwards, never past the newest byte. */
    lr_rtc_mark_uploaded(&h, 1);
    assert(h.uploaded == 3);
    lr_rtc_mark_uploaded(&h, 99);
    assert(h.uploaded == 4);

    /* Unsent bytes the ring has since overwritten are simply gone. */
    append_str(&h.ring, data, "456789AB");   /* holds 4..11 */
    lr_rtc_mark_uploaded(&h, 2);
    assert(h.uploaded == 4 && lr_rtc_unsent_from(&h) == 4);
    append_str(&h.ring, data, "CD");         /* holds 6..13 */
    assert(lr_rtc_unsent_from(&h) == 6);
}

/* ---- header ---- */

static void test_header_line(void)
{
    char out[160];
    int n = lr_format_header(out, sizeof out, "1.41.0", 7, "brownout", "timer",
                             1790000000u);
    assert(n == (int)strlen(out));
    assert(strcmp(out, "# tesserae-log v1 fw=1.41.0 boot=7 reset=brownout "
                       "wake=timer epoch=1790000000\n") == 0);
    lr_format_header(out, sizeof out, "0.0.0-dev", 1, "poweron", "none", 0);
    assert(strcmp(out, "# tesserae-log v1 fw=0.0.0-dev boot=1 reset=poweron "
                       "wake=none epoch=0\n") == 0);
}

/* ---- cleaning and redaction ---- */

static void test_clean_strips_esp_log_colour(void)
{
    char out[128];
    size_t n = clean_str("\033[0;32mI (1234) rest: <- 200 (57 bytes)\033[0m\n",
                         out, sizeof out);
    assert(strcmp(out, "I (1234) rest: <- 200 (57 bytes)\n") == 0);
    assert(n == strlen(out));

    /* Colour code cut off by line truncation. */
    clean_str("W (1) x: cut\033[0\n", out, sizeof out);
    assert(strcmp(out, "W (1) x: cut\n") == 0);
}

static void test_clean_drops_cr_and_masks_control_bytes(void)
{
    char out[64];
    clean_str("a\rb\tc\x01" "d\x7f\n", out, sizeof out);
    assert(strcmp(out, "ab\tc?d?\n") == 0);
}

static void test_redacts_url_query_strings(void)
{
    char out[256];
    clean_str("E (9) main: frame fetch failed for "
              "https://tesserae.example/r/abc.bin?sig=deadbeef&exp=1790000000\n",
              out, sizeof out);
    assert(strcmp(out, "E (9) main: frame fetch failed for "
                       "https://tesserae.example/r/abc.bin?<redacted>\n") == 0);

    /* Text after the URL survives; quotes end it. */
    clean_str("url=\"http://10.0.0.2:5555/f?t=1\" retry", out, sizeof out);
    assert(strcmp(out, "url=\"http://10.0.0.2:5555/f?<redacted>\" retry") == 0);

    /* Any case, several URLs, and no query means no change. */
    clean_str("HTTPS://a/b?x=1 http://c/d http://e?f", out, sizeof out);
    assert(strcmp(out, "HTTPS://a/b?<redacted> http://c/d http://e?<redacted>") == 0);

    /* A query at the very end of a line without a newline. */
    clean_str("GET https://h/p?token=abc", out, sizeof out);
    assert(strcmp(out, "GET https://h/p?<redacted>") == 0);
}

static void test_redacts_bearer_tokens(void)
{
    char out[128];
    clean_str("hdr Authorization: Bearer eyJ.abc-DEF_123 next\n", out, sizeof out);
    assert(strcmp(out, "hdr Authorization: Bearer <redacted> next\n") == 0);
    clean_str("BEARER xyz", out, sizeof out);
    assert(strcmp(out, "BEARER <redacted>") == 0);
    /* No token after the word: left alone. */
    clean_str("Bearer ", out, sizeof out);
    assert(strcmp(out, "Bearer ") == 0);
}

static void test_clean_truncates_but_keeps_the_newline(void)
{
    char out[8];
    size_t n = clean_str("0123456789\n", out, sizeof out);
    assert(n == 7);
    assert(strcmp(out, "012345\n") == 0);

    /* Redaction that would overflow is cut, not written past the end. */
    n = clean_str("http://a?b\n", out, sizeof out);
    assert(n == 7 && out[6] == '\n' && out[7] == '\0');
    assert(strncmp(out, "http://", 6) == 0);

    char one[1];
    assert(clean_str("abc\n", one, sizeof one) == 0 && one[0] == '\0');
    char two[2];
    assert(clean_str("abc\n", two, sizeof two) == 1 && strcmp(two, "\n") == 0);
}

/* ---- batch ---- */

static void test_batch_joins_contiguous_segments(void)
{
    char buf[64];
    lr_batch_t b;
    lr_batch_init(&b, buf, sizeof buf);
    assert(lr_batch_add(&b, 100, "one\n", 4));
    assert(lr_batch_add(&b, 104, "two\n", 4));
    assert(lr_batch_finish(&b, 65536) == 8);
    assert(memcmp(buf, "one\ntwo\n", 8) == 0);
}

static void test_batch_marks_a_gap_and_skips_an_overlap(void)
{
    char buf[128];
    lr_batch_t b;
    lr_batch_init(&b, buf, sizeof buf);
    lr_batch_add(&b, 0, "abc", 3);            /* no trailing newline */
    lr_batch_add(&b, 10, "late\n", 5);        /* 7 bytes lost */
    lr_batch_add(&b, 12, "te\nnew\n", 7);     /* overlaps "te\n" */
    size_t n = lr_batch_finish(&b, 65536);
    buf[n] = '\0';
    assert(strcmp(buf, "abc\n# ... 7 bytes dropped\nlate\nnew\n") == 0);

    /* Entirely old: nothing added. */
    assert(lr_batch_add(&b, 0, "abc", 3));
    assert(b.len == n);
}

static void test_batch_reports_running_out_of_room(void)
{
    char buf[6];
    lr_batch_t b;
    lr_batch_init(&b, buf, sizeof buf);
    assert(!lr_batch_add(&b, 0, "0123456789", 10));
    assert(b.len == sizeof buf && memcmp(buf, "012345", 6) == 0);
}

static void test_batch_front_truncation_marker(void)
{
    char buf[512];
    char body[401];
    size_t len = 0;
    for (int i = 0; i < 40; i++)
        len += (size_t)snprintf(body + len, sizeof body - len, "line %03d.\n", i);
    assert(len == 400);   /* 40 lines of 10 bytes */

    lr_batch_t b;
    lr_batch_init(&b, buf, sizeof buf);
    lr_batch_add(&b, 0, body, len);
    size_t n = lr_batch_finish(&b, 200);
    assert(n <= 200);
    buf[n] = '\0';

    /* Starts with the marker, then whole lines, ending on the last one. */
    unsigned long dropped = 0;
    assert(sscanf(buf, "# ... %lu bytes dropped\n", &dropped) == 1);
    const char *rest = strchr(buf, '\n') + 1;
    assert(strncmp(rest, "line ", 5) == 0);
    assert(dropped % 10 == 0);
    assert(strcmp(body + dropped, rest) == 0);
    assert(strcmp(buf + n - 10, "line 039.\n") == 0);

    /* Already within the limit: untouched. */
    lr_batch_init(&b, buf, sizeof buf);
    lr_batch_add(&b, 0, body, 50);
    assert(lr_batch_finish(&b, 200) == 50);
}

static void test_batch_truncation_cap_is_64k(void)
{
    static char buf[70 * 1024];
    static char body[70 * 1024];
    size_t len = 0;
    while (len + 50 <= sizeof body) {
        memset(body + len, 'x', 49);
        body[len + 49] = '\n';
        len += 50;
    }
    lr_batch_t b;
    lr_batch_init(&b, buf, sizeof buf);
    lr_batch_add(&b, 0, body, len);
    size_t n = lr_batch_finish(&b, 64 * 1024);
    assert(n <= 64 * 1024 && n > 64 * 1024 - LR_MARKER_MAX - 50);
    assert(strncmp(buf, "# ... ", 6) == 0);
    assert(buf[n - 1] == '\n');
}

/* ---- two wakes end to end, the way log_capture.c drives it ---- */

typedef struct {
    lr_rtc_hdr_t h;
    char         data[64];
} fake_rtc_t;

/* One boot: set aside the unsent tail, then log `lines`. Returns the carry. */
static size_t boot_and_log(fake_rtc_t *rtc, const char *lines, char *carry,
                           uint32_t *carry_from, uint32_t *boot_total)
{
    lr_rtc_begin_boot(&rtc->h, sizeof rtc->data);
    *boot_total = rtc->h.ring.total;
    uint32_t from = lr_rtc_unsent_from(&rtc->h);
    size_t n = lr_ring_read(&rtc->h.ring, rtc->data, from, *boot_total, carry,
                            sizeof rtc->data, carry_from);
    char hdr[96];
    int hn = lr_format_header(hdr, sizeof hdr, "t", rtc->h.boot, "deepsleep",
                              "timer", 0);
    lr_ring_append(&rtc->h.ring, rtc->data, hdr, (size_t)hn);
    append_str(&rtc->h.ring, rtc->data, lines);
    return n;
}

static void test_previous_wake_tail_is_uploaded_once(void)
{
    fake_rtc_t rtc;
    memset(&rtc, 0xEE, sizeof rtc);   /* power-on garbage */
    char carry[64], batch[256];
    uint32_t carry_from, boot_total;

    /* Wake 1: nothing to carry; logs its paint, never uploads. */
    size_t cn = boot_and_log(&rtc, "paint failed\n", carry, &carry_from, &boot_total);
    assert(cn == 0 && boot_total == 0);
    uint32_t wake1_end = rtc.h.ring.total;

    /* Wake 2: the ring still holds wake 1's tail; the server asks. */
    cn = boot_and_log(&rtc, "status ok\n", carry, &carry_from, &boot_total);
    assert(boot_total == wake1_end);
    assert(cn > 0 && carry_from + cn == boot_total);
    assert(memcmp(carry + cn - 13, "paint failed\n", 13) == 0);

    uint32_t to = rtc.h.ring.total;
    lr_batch_t b;
    lr_batch_init(&b, batch, sizeof batch);
    lr_batch_add(&b, carry_from, carry, cn);
    char chunk[16];
    uint32_t pos = boot_total, got_from;
    size_t got;
    while ((got = lr_ring_read(&rtc.h.ring, rtc.data, pos, to, chunk,
                               sizeof chunk, &got_from)) > 0) {
        lr_batch_add(&b, got_from, chunk, got);
        pos = got_from + (uint32_t)got;
    }
    size_t n = lr_batch_finish(&b, 65536);
    batch[n] = '\0';
    assert(strstr(batch, "paint failed\n") != NULL);
    assert(strstr(batch, "status ok\n") != NULL);
    lr_rtc_mark_uploaded(&rtc.h, to);

    /* Rest of wake 2 after the upload. */
    append_str(&rtc.h.ring, rtc.data, "sleep\n");

    /* Wake 3: only what wake 2 logged after its upload is unsent. */
    cn = boot_and_log(&rtc, "x\n", carry, &carry_from, &boot_total);
    assert(cn == 6 && memcmp(carry, "sleep\n", 6) == 0);
    assert(carry_from == to);
}

int main(void)
{
    test_ring_holds_newest_bytes_across_a_wrap();
    test_ring_append_longer_than_cap_keeps_the_tail();
    test_ring_read_clamps_and_chunks();
    test_ring_offsets_survive_uint32_wrap();
    test_ring_sane_rejects_garbage();
    test_rtc_power_on_garbage_is_reinitialised();
    test_rtc_state_is_kept_across_boots();
    test_rtc_uploaded_ahead_of_total_is_invalid();
    test_rtc_uploaded_up_to();
    test_header_line();
    test_clean_strips_esp_log_colour();
    test_clean_drops_cr_and_masks_control_bytes();
    test_redacts_url_query_strings();
    test_redacts_bearer_tokens();
    test_clean_truncates_but_keeps_the_newline();
    test_batch_joins_contiguous_segments();
    test_batch_marks_a_gap_and_skips_an_overlap();
    test_batch_reports_running_out_of_room();
    test_batch_front_truncation_marker();
    test_batch_truncation_cap_is_64k();
    test_previous_wake_tail_is_uploaded_once();
    puts("log_ring: all tests passed");
    return 0;
}
