// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"

static diag_state_t fresh(uint32_t seed)
{
    diag_state_t s;
    memset(&s, 0xC3, sizeof s);   /* power-on garbage */
    diag_boot(&s, DIAG_RESET_NONE, seed, 0);
    return s;
}

static void test_power_on_starts_clean_with_seeded_ids(void)
{
    diag_state_t s = fresh(1000);
    assert(s.magic == DIAG_MAGIC && s.next_id == 1000);
    assert(!diag_get(&s, NULL));
}

static void test_abnormal_reset_latches_on_boot(void)
{
    diag_state_t s;
    memset(&s, 0, sizeof s);
    diag_boot(&s, DIAG_RESET_BROWNOUT, 500, 1790000000u);
    diag_report_t r;
    assert(diag_get(&s, &r));
    assert(r.id == 500 && r.reset == DIAG_RESET_BROWNOUT);
    assert(r.paint == DIAG_PAINT_NONE && r.at == 1790000000u);
}

static void test_paint_latch_reports_once_per_change(void)
{
    diag_state_t s = fresh(7);
    assert(diag_latch_paint(&s, DIAG_PAINT_READY_TIMEOUT, 0));
    /* A wait loop timing out on every command: no further changes. */
    assert(!diag_latch_paint(&s, DIAG_PAINT_READY_TIMEOUT, 5));
    /* The first failure of a report is the one it keeps. */
    assert(!diag_latch_paint(&s, DIAG_PAINT_REFRESH_TIMEOUT, 6));
    assert(!diag_latch_paint(&s, DIAG_PAINT_NONE, 6));
    diag_report_t r;
    assert(diag_get(&s, &r));
    assert(r.id == 7 && r.paint == DIAG_PAINT_READY_TIMEOUT && r.reset == DIAG_RESET_NONE);
    /* Clock unknown at the latch, known later: the later time fills in. */
    assert(r.at == 5);
}

static void test_clear_needs_the_delivered_id(void)
{
    diag_state_t s = fresh(41);
    diag_latch_paint(&s, DIAG_PAINT_INIT_FAILED, 100);
    diag_clear(&s, 40);
    assert(diag_get(&s, NULL));
    diag_clear(&s, 41);
    assert(!diag_get(&s, NULL));

    /* The next failure is a new report with a new id. */
    diag_latch_paint(&s, DIAG_PAINT_REFRESH_TIMEOUT, 200);
    diag_report_t r;
    assert(diag_get(&s, &r));
    assert(r.id == 42 && r.paint == DIAG_PAINT_REFRESH_TIMEOUT && r.at == 200);
}

static void test_report_survives_reboots_until_delivered(void)
{
    diag_state_t s = fresh(9);
    diag_latch_paint(&s, DIAG_PAINT_REFRESH_TIMEOUT, 300);

    /* Deep-sleep wake: kept as is. */
    diag_boot(&s, DIAG_RESET_NONE, 12345, 400);
    diag_report_t r;
    assert(diag_get(&s, &r) && r.id == 9 && r.at == 300);

    /* A brownout before it was delivered joins the same report. */
    diag_boot(&s, DIAG_RESET_BROWNOUT, 12345, 500);
    assert(diag_get(&s, &r));
    assert(r.id == 9 && r.paint == DIAG_PAINT_REFRESH_TIMEOUT);
    assert(r.reset == DIAG_RESET_BROWNOUT && r.at == 300);

    diag_clear(&s, 9);
    diag_boot(&s, DIAG_RESET_WATCHDOG, 12345, 600);
    assert(diag_get(&s, &r) && r.id == 10 && r.reset == DIAG_RESET_WATCHDOG);
    assert(r.paint == DIAG_PAINT_NONE);
}

static void test_corrupt_fields_restart_the_state(void)
{
    diag_state_t s = fresh(3);
    diag_latch_paint(&s, DIAG_PAINT_INIT_FAILED, 1);
    s.paint = 9;
    assert(!diag_get(&s, NULL));
    diag_boot(&s, DIAG_RESET_NONE, 77, 0);
    assert(!diag_get(&s, NULL) && s.next_id == 77);
}

static void test_contract_names(void)
{
    assert(strcmp(diag_paint_name(DIAG_PAINT_INIT_FAILED), "init_failed") == 0);
    assert(strcmp(diag_paint_name(DIAG_PAINT_READY_TIMEOUT), "ready_timeout") == 0);
    assert(strcmp(diag_paint_name(DIAG_PAINT_REFRESH_TIMEOUT), "refresh_timeout") == 0);
    assert(diag_paint_name(DIAG_PAINT_NONE) == NULL);
    assert(strcmp(diag_reset_name(DIAG_RESET_BROWNOUT), "brownout") == 0);
    assert(strcmp(diag_reset_name(DIAG_RESET_PANIC), "panic") == 0);
    assert(strcmp(diag_reset_name(DIAG_RESET_WATCHDOG), "watchdog") == 0);
    assert(diag_reset_name(DIAG_RESET_NONE) == NULL);
}

int main(void)
{
    test_power_on_starts_clean_with_seeded_ids();
    test_abnormal_reset_latches_on_boot();
    test_paint_latch_reports_once_per_change();
    test_clear_needs_the_delivered_id();
    test_report_survives_reboots_until_delivered();
    test_corrupt_fields_restart_the_state();
    test_contract_names();
    puts("diag: all tests passed");
    return 0;
}
