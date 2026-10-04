// SPDX-License-Identifier: AGPL-3.0-or-later
// Modified 2026-10-01: describe the bounded PicPak capture buffers.
/*
 * log_ring.h: the pure half of device log capture (log_capture.h). No ESP-IDF
 * here, so the host tests can drive the ring, the redaction and the batch
 * builder directly (test/test_log_ring.c).
 *
 * Offsets. Every captured byte has a position in one stream that runs across
 * wakes, counted by a uint32 that is allowed to wrap. Retained and temporary
 * buffers are windows
 * onto that stream, which is what lets an upload say "everything from offset
 * X" without caring which buffer still holds it. Compare offsets with
 * lr_before(), never with <.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One formatted log line before cleaning. The capture hook omits longer lines. */
#define LR_LINE_MAX    256
/* Room for one "# ... N bytes dropped" marker, newline before and after. */
#define LR_MARKER_MAX  48

/* a comes before b in stream order (wrap-safe for gaps under 2 GB). */
static inline bool lr_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

/* ---- ring over the stream ---- */

typedef struct {
    uint32_t total;   /* stream offset one past the newest byte held */
    uint32_t len;     /* bytes held, <= cap */
    uint32_t head;    /* index the next byte is written to, < cap */
    uint32_t cap;
} lr_ring_t;

/* Empty ring of `cap` bytes whose next byte will sit at stream offset `total`. */
void lr_ring_reset(lr_ring_t *r, uint32_t cap, uint32_t total);

/* Header fields consistent with a ring of `cap` bytes. */
bool lr_ring_sane(const lr_ring_t *r, uint32_t cap);

/* Append n bytes; the oldest are overwritten once the ring is full. */
void lr_ring_append(lr_ring_t *r, char *data, const char *src, size_t n);

/* Stream offset of the oldest byte still held. */
static inline uint32_t lr_ring_oldest(const lr_ring_t *r)
{
    return r->total - r->len;
}

/* Copy stream bytes [from, to) that the ring still holds into out, at most
 * out_cap of them, oldest first. `from` is clamped up to the oldest byte held
 * and `to` down to the newest. Returns the byte count; *from_out receives the
 * offset actually copied from. */
size_t lr_ring_read(const lr_ring_t *r, const char *data,
                    uint32_t from, uint32_t to,
                    char *out, size_t out_cap, uint32_t *from_out);

/* ---- retained state (RTC_NOINIT) ---- */

#define LR_RTC_MAGIC 0x544C4F47u   /* 'TLOG' */

typedef struct {
    uint32_t  magic;
    uint32_t  boot;       /* boots since the ring was last initialised */
    uint32_t  uploaded;   /* stream offset up to which a server has the bytes */
    lr_ring_t ring;
    uint32_t  magic_inv;  /* ~magic: power-on garbage rarely matches both */
} lr_rtc_hdr_t;

/* Start a boot on retained state: validate it (reinitialising anything that
 * does not hold together, as after a power-on), then count the boot. Returns
 * true when the retained state was kept. */
bool lr_rtc_begin_boot(lr_rtc_hdr_t *h, uint32_t cap);

/* First stream offset that is both unsent and still held by the ring. */
uint32_t lr_rtc_unsent_from(const lr_rtc_hdr_t *h);

/* A server stored everything before `upto`. Only moves forward, never past
 * the newest byte. */
void lr_rtc_mark_uploaded(lr_rtc_hdr_t *h, uint32_t upto);

/* ---- lines ---- */

/* Clean one chunk of log output for capture: drop ANSI escape sequences and
 * carriage returns, replace other control bytes (bar newline and tab) with
 * '?', replace the query string of every http:// or https:// URL with
 * "?<redacted>", credential fields, URL user information and pairing paths, and every
 * "Bearer <token>" with "Bearer <redacted>". Output
 * is NUL-terminated and cut to fit cap; a newline that ended the input is kept
 * even when the rest is cut. Returns the length written. */
size_t lr_clean(const char *in, size_t n, char *out, size_t cap);

/* The v1 wake header:
 *   # tesserae-log v1 fw=<ver> boot=<n> reset=<reason> wake=<cause> epoch=<t>
 * newline included. Returns the length written (snprintf semantics). */
int lr_format_header(char *out, size_t cap, const char *fw, uint32_t boot,
                     const char *reset, const char *wake, uint32_t epoch);

/* ---- upload batch ---- */

typedef struct {
    char    *buf;
    size_t   cap;
    size_t   len;
    uint32_t next;      /* stream offset the next segment should start at */
    bool     started;
} lr_batch_t;

void lr_batch_init(lr_batch_t *b, char *buf, size_t cap);

/* Append stream bytes starting at offset `from`, in stream order. A segment
 * that starts past the end of the previous one writes a
 * "# ... N bytes dropped" line for the gap first; one that overlaps skips the
 * bytes already added. Returns false when the buffer ran out of room (what
 * fitted is kept). */
bool lr_batch_add(lr_batch_t *b, uint32_t from, const char *p, size_t n);

/* Fit the batch into `limit` bytes by dropping from the front: the cut moves
 * forward to the next line start and a "# ... N bytes dropped" line takes its
 * place. Returns the final length. */
size_t lr_batch_finish(lr_batch_t *b, size_t limit);
