// test_battpct.c — host unit test for the battery %-curve (PhotoPainter piecewise curve).
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include "battpct.h"

int main(void) {
    assert(battpct(3000) == 0);     // below window -> clamp 0
    assert(battpct(3400) == 0);
    assert(battpct(3700) == 25);
    assert(battpct(3900) == 65);
    assert(battpct(4000) == 80);
    assert(battpct(4150) == 95);
    assert(battpct(4164) == 96);    // nearly full -> 9x%, not pegged 100
    assert(battpct(4200) == 100);
    assert(battpct(4300) == 100);   // above window -> clamp 100
    printf("PASS\n");
    return 0;
}
