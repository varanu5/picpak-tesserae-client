// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "log_capture.h"
#include "log_ring.h"
#include "defaults.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "logcap";
RTC_NOINIT_ATTR static lr_rtc_hdr_t s_rtc;
RTC_NOINIT_ATTR static char s_ring[LOG_CAPTURE_RING_BYTES];
RTC_NOINIT_ATTR static diag_state_t s_diag;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready, s_busy;
static uint32_t s_skipped, s_boot_start;
static char *s_carry;
static uint32_t s_carry_len, s_carry_from;
static vprintf_like_t s_previous;
static char s_line[LR_LINE_MAX], s_clean[2 * LR_LINE_MAX];

static uint32_t epoch_now(void) {
    time_t now = time(NULL);
    return now >= CLOCK_SANE_EPOCH && now < 4102444800LL ? (uint32_t)now : 0;
}

static void append(const char *line, size_t len) {
    lr_ring_append(&s_rtc.ring, s_ring, line, len);
}

static int capture(const char *fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int result = s_previous ? s_previous(fmt, args) : vprintf(fmt, args);
    portENTER_CRITICAL_SAFE(&s_lock);
    bool busy = s_busy;
    if (busy) s_skipped++;
    else s_busy = true;
    portEXIT_CRITICAL_SAFE(&s_lock);
    if (busy) { va_end(copy); return result; }

    int n = vsnprintf(s_line, sizeof s_line, fmt, copy);
    va_end(copy);
    size_t clean = 0;
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof s_line) {
            strcpy(s_line, "# ... oversized log line omitted\n");
            len = strlen(s_line);
        }
        clean = lr_clean(s_line, len, s_clean, sizeof s_clean);
    }
    portENTER_CRITICAL_SAFE(&s_lock);
    uint32_t skipped = s_skipped;
    s_skipped = 0;
    portEXIT_CRITICAL_SAFE(&s_lock);
    char note[64];
    int count = skipped ? snprintf(note, sizeof note, "# ... %lu lines not captured\n",
                                   (unsigned long)skipped) : 0;
    portENTER_CRITICAL_SAFE(&s_lock);
    if (count > 0 && (size_t)count < sizeof note) append(note, (size_t)count);
    if (clean) append(s_clean, clean);
    s_busy = false;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return result;
}

static const char *reset_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_SW: return "sw";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        default: return "other";
    }
}

void log_capture_init(void) {
    if (s_ready) return;
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON) {
        memset(&s_rtc, 0, sizeof s_rtc);
        memset(&s_diag, 0, sizeof s_diag);
    }
    bool retained = lr_rtc_begin_boot(&s_rtc, sizeof s_ring);
    s_boot_start = s_rtc.ring.total;
    uint32_t from = lr_rtc_unsent_from(&s_rtc);
    uint32_t unsent = s_boot_start - from;
    if (unsent) {
        s_carry = malloc(unsent);
        if (s_carry)
            s_carry_len = (uint32_t)lr_ring_read(&s_rtc.ring, s_ring, from,
                s_boot_start, s_carry, unsent, &s_carry_from);
    }
    diag_reset_t reset = DIAG_RESET_NONE;
    if (reason == ESP_RST_BROWNOUT) reset = DIAG_RESET_BROWNOUT;
    else if (reason == ESP_RST_PANIC) reset = DIAG_RESET_PANIC;
    else if (reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT)
        reset = DIAG_RESET_WATCHDOG;
    diag_boot(&s_diag, reset, esp_random(), epoch_now());

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const char *wake = cause == ESP_SLEEP_WAKEUP_TIMER ? "timer"
                     : cause == ESP_SLEEP_WAKEUP_GPIO ? "gpio" : "other";
    char header[160];
    int n = lr_format_header(header, sizeof header, FW_VERSION, s_rtc.boot,
                             reset_name(reason), wake, epoch_now());
    if (n > 0 && (size_t)n < sizeof header) append(header, (size_t)n);
    s_previous = esp_log_set_vprintf(capture);
    s_ready = true;
    ESP_LOGI(TAG, "log ring %u bytes (%s), previous wake %lu bytes",
             (unsigned)sizeof s_ring, retained ? "retained" : "new",
             (unsigned long)s_carry_len);
}

void log_capture_paint_error(diag_paint_t error) {
    if (!s_ready) return;
    uint32_t now = epoch_now();
    portENTER_CRITICAL(&s_lock);
    bool changed = diag_latch_paint(&s_diag, error, now);
    portEXIT_CRITICAL(&s_lock);
    if (changed) ESP_LOGW(TAG, "paint failure recorded: %s", diag_paint_name(error));
}

bool log_capture_status(char *body, size_t cap, uint32_t *report_id) {
    if (!s_ready || !body || !cap) return false;
    size_t len = strnlen(body, cap);
    if (!len || len >= cap || body[len - 1] != '}') return false;
    diag_report_t report;
    portENTER_CRITICAL(&s_lock);
    bool pending = diag_get(&s_diag, &report);
    portEXIT_CRITICAL(&s_lock);
    char diagnostic[192] = "";
    if (pending) {
        const char *paint = diag_paint_name(report.paint);
        const char *reset = diag_reset_name(report.reset);
        snprintf(diagnostic, sizeof diagnostic,
                 ",\"diag\":{\"id\":%lu,\"at\":%lu%s%s%s%s%s%s}",
                 (unsigned long)report.id, (unsigned long)report.at,
                 paint ? ",\"paint_error\":\"" : "", paint ? paint : "", paint ? "\"" : "",
                 reset ? ",\"reset\":\"" : "", reset ? reset : "", reset ? "\"" : "");
    }
    int n = snprintf(body + len - 1, cap - len + 1,
                     "%s\"logs\":{\"schema\":1,\"ring_bytes\":%u}%s}",
                     len > 2 ? "," : "", LOG_CAPTURE_RING_BYTES, diagnostic);
    if (n < 0 || (size_t)n >= cap - len + 1) {
        body[len - 1] = '}';
        body[len] = '\0';
        return false;
    }
    if (pending && report_id) *report_id = report.id;
    return pending;
}

void log_capture_ack_report(uint32_t report_id) {
    portENTER_CRITICAL(&s_lock);
    diag_clear(&s_diag, report_id);
    portEXIT_CRITICAL(&s_lock);
}

bool log_capture_upload(log_upload_fn send, void *context) {
    if (!s_ready || !send) return false;
    portENTER_CRITICAL(&s_lock);
    uint32_t to = s_rtc.ring.total;
    uint32_t from = s_rtc.uploaded;
    portEXIT_CRITICAL(&s_lock);
    if (to == from) return true;
    size_t cap = s_carry_len + sizeof s_ring + 3 * LR_MARKER_MAX;
    char *body = malloc(cap + 1);
    if (!body) {
        ESP_LOGW(TAG, "log upload skipped: insufficient memory");
        return false;
    }
    lr_batch_t batch;
    lr_batch_init(&batch, body, cap);
    batch.started = true;
    batch.next = from;
    bool fits = true;
    if (s_carry_len) {
        uint32_t skip = lr_before(s_carry_from, from) ? from - s_carry_from : 0;
        if (skip < s_carry_len)
            fits = lr_batch_add(&batch, s_carry_from + skip, s_carry + skip, s_carry_len - skip);
    }
    static char chunk[512];
    uint32_t pos = lr_before(from, s_boot_start) ? s_boot_start : from;
    while (fits && lr_before(pos, to)) {
        uint32_t copied_from;
        portENTER_CRITICAL(&s_lock);
        size_t n = lr_ring_read(&s_rtc.ring, s_ring, pos, to, chunk, sizeof chunk, &copied_from);
        portEXIT_CRITICAL(&s_lock);
        if (!n) { fits = false; break; }
        fits = lr_batch_add(&batch, copied_from, chunk, n);
        pos = copied_from + (uint32_t)n;
    }
    body[batch.len] = '\0';
    bool accepted = fits && batch.len > 0 && send(body, batch.len, context);
    free(body);
    if (!accepted) {
        ESP_LOGW(TAG, "log upload failed, retained bytes remain eligible");
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    lr_rtc_mark_uploaded(&s_rtc, to);
    portEXIT_CRITICAL(&s_lock);
    free(s_carry);
    s_carry = NULL;
    s_carry_len = 0;
    ESP_LOGI(TAG, "uploaded %u log bytes", (unsigned)batch.len);
    return true;
}
