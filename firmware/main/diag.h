// SPDX-License-Identifier: AGPL-3.0-or-later
// Modified 2026-10-01: include failed display transfers and shutdowns.
/*
 * diag.h: detected-failure latch, reported on the next /status as "diag"
 * (device log upload plan, Protocol v1). Pure logic over a state struct the
 * firmware keeps in RTC_NOINIT memory; the host tests drive it
 * directly (test/test_diag.c).
 *
 * One latch at a time. The first failure after a delivered report opens it
 * with a new id; later failures before it is delivered only fill in a field it
 * does not have yet, so a panel failing every wake while offline reports one
 * failure, not one per wake. A status POST that carried the report and came
 * back 2xx clears it (diag_clear with the id that was sent).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DIAG_PAINT_NONE = 0,
    DIAG_PAINT_INIT_FAILED,      /* controller initialization failed */
    DIAG_PAINT_READY_TIMEOUT,    /* controller busy / HRDY line stuck */
    DIAG_PAINT_REFRESH_TIMEOUT,  /* refresh never reported done */
    DIAG_PAINT_DISPLAY_FAILED,  /* display transfer or shutdown failed */
    DIAG_PAINT_COUNT
} diag_paint_t;

typedef enum {
    DIAG_RESET_NONE = 0,         /* power-on, deep-sleep wake, software reset */
    DIAG_RESET_BROWNOUT,
    DIAG_RESET_PANIC,
    DIAG_RESET_WATCHDOG,
    DIAG_RESET_COUNT
} diag_reset_t;

#define DIAG_MAGIC 0x44494147u   /* 'DIAG' */

typedef struct {
    uint32_t magic;
    uint32_t next_id;    /* id the next opened latch takes */
    uint32_t id;         /* id of the open latch */
    uint32_t at;         /* epoch of the latch, 0 if the clock was unknown */
    uint8_t  pending;    /* 1 while a report waits for delivery */
    uint8_t  paint;      /* diag_paint_t */
    uint8_t  reset;      /* diag_reset_t */
    uint8_t  pad;
    uint32_t magic_inv;
} diag_state_t;

typedef struct {
    uint32_t     id;
    diag_paint_t paint;
    diag_reset_t reset;
    uint32_t     at;
} diag_report_t;

/* Start of a boot: validate the retained state (a power-on or anything that
 * does not hold together restarts it, ids from `seed`), then latch `reset` if
 * this boot was abnormal. */
void diag_boot(diag_state_t *s, diag_reset_t reset, uint32_t seed, uint32_t now);

/* Record a paint failure. Returns true when this changed the report (so the
 * caller logs once, not once per timed-out poll). */
bool diag_latch_paint(diag_state_t *s, diag_paint_t paint, uint32_t now);

/* The report waiting for delivery, if any. */
bool diag_get(const diag_state_t *s, diag_report_t *out);

/* A status carrying report `id` was accepted. A latch reopened since (a
 * different id) stays. */
void diag_clear(diag_state_t *s, uint32_t id);

/* Contract names: "init_failed", "ready_timeout", "refresh_timeout", "display_failed", and
 * "brownout" / "panic" / "watchdog". NULL for NONE. */
const char *diag_paint_name(diag_paint_t p);
const char *diag_reset_name(diag_reset_t r);
