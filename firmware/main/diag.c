// SPDX-License-Identifier: AGPL-3.0-or-later
// Modified 2026-10-01: include failed display transfers and shutdowns.
/*
 * diag.c: detected-failure latch. Pure C; see diag.h.
 */
#include "diag.h"

#include <string.h>

static bool valid(const diag_state_t *s)
{
    return s->magic == DIAG_MAGIC && s->magic_inv == (uint32_t)~DIAG_MAGIC &&
           s->pending <= 1 && s->paint < DIAG_PAINT_COUNT &&
           s->reset < DIAG_RESET_COUNT;
}

/* Open a latch if none is waiting, then fill in what it lacks. */
static bool latch(diag_state_t *s, diag_paint_t paint, diag_reset_t reset,
                  uint32_t now)
{
    bool changed = false;
    if (!s->pending) {
        s->pending = 1;
        s->id = s->next_id++;
        s->paint = DIAG_PAINT_NONE;
        s->reset = DIAG_RESET_NONE;
        s->at = now;
        changed = true;
    }
    if (paint != DIAG_PAINT_NONE && s->paint == DIAG_PAINT_NONE) {
        s->paint = (uint8_t)paint;
        changed = true;
    }
    if (reset != DIAG_RESET_NONE && s->reset == DIAG_RESET_NONE) {
        s->reset = (uint8_t)reset;
        changed = true;
    }
    if (s->at == 0) s->at = now;
    return changed;
}

void diag_boot(diag_state_t *s, diag_reset_t reset, uint32_t seed, uint32_t now)
{
    if (!valid(s)) {
        memset(s, 0, sizeof *s);
        s->magic = DIAG_MAGIC;
        s->magic_inv = (uint32_t)~DIAG_MAGIC;
        s->next_id = seed;
    }
    if (reset != DIAG_RESET_NONE && reset < DIAG_RESET_COUNT)
        latch(s, DIAG_PAINT_NONE, reset, now);
}

bool diag_latch_paint(diag_state_t *s, diag_paint_t paint, uint32_t now)
{
    if (paint == DIAG_PAINT_NONE || paint >= DIAG_PAINT_COUNT) return false;
    return latch(s, paint, DIAG_RESET_NONE, now);
}

bool diag_get(const diag_state_t *s, diag_report_t *out)
{
    if (!valid(s) || !s->pending) return false;
    if (out) {
        out->id = s->id;
        out->paint = (diag_paint_t)s->paint;
        out->reset = (diag_reset_t)s->reset;
        out->at = s->at;
    }
    return true;
}

void diag_clear(diag_state_t *s, uint32_t id)
{
    if (!s->pending || s->id != id) return;
    s->pending = 0;
    s->paint = DIAG_PAINT_NONE;
    s->reset = DIAG_RESET_NONE;
    s->at = 0;
}

const char *diag_paint_name(diag_paint_t p)
{
    switch (p) {
    case DIAG_PAINT_INIT_FAILED:     return "init_failed";
    case DIAG_PAINT_READY_TIMEOUT:   return "ready_timeout";
    case DIAG_PAINT_REFRESH_TIMEOUT: return "refresh_timeout";
    case DIAG_PAINT_DISPLAY_FAILED:  return "display_failed";
    default:                         return NULL;
    }
}

const char *diag_reset_name(diag_reset_t r)
{
    switch (r) {
    case DIAG_RESET_BROWNOUT: return "brownout";
    case DIAG_RESET_PANIC:    return "panic";
    case DIAG_RESET_WATCHDOG: return "watchdog";
    default:                  return NULL;
    }
}
