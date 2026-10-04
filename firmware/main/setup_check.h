// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdbool.h>

static inline bool setup_should_reopen(bool just_saved, bool wifi_ok, bool transport_ok) {
    return just_saved && (!wifi_ok || !transport_ok);
}
