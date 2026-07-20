// battpct.h — pure battery mV->% curve (host-testable, no ESP deps).
// PhotoPainter Li-Po discharge curve: piecewise-linear, non-linear vs charge (flat near the top,
// steep near empty). Chosen over the stock PicPak linear LUT for granular high-end readings
// (100% only at 4200mV, so a nearly-full cell reads 9x% instead of pegging at 100%).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once

static inline int battpct(int mv) {
    static const struct { int mv; int pct; } CURVE[] = {
        {4200, 100}, {4150, 95}, {4100, 90}, {4050, 85}, {4000, 80}, {3950, 75},
        {3900, 65}, {3850, 55}, {3800, 45}, {3750, 35}, {3700, 25}, {3650, 15},
        {3600, 8}, {3550, 5}, {3500, 2}, {3400, 0},
    };
    const int n = (int)(sizeof(CURVE) / sizeof(CURVE[0]));
    if (mv >= CURVE[0].mv) return 100;
    if (mv <= CURVE[n - 1].mv) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= CURVE[i].mv) {
            int span_mv  = CURVE[i - 1].mv  - CURVE[i].mv;
            int span_pct = CURVE[i - 1].pct - CURVE[i].pct;
            int over_mv  = mv - CURVE[i].mv;
            return CURVE[i].pct + (over_mv * span_pct + span_mv / 2) / span_mv;
        }
    }
    return 0;
}
