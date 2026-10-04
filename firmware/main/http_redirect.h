// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdbool.h>
#include <string.h>
#include <strings.h>

// Authenticated redirects must retain the original scheme and authority.
static inline bool http_redirect_allowed(const char *origin, const char *location) {
    if (!origin || !origin[0] || !location || !location[0]) return false;
    for (const unsigned char *p = (const unsigned char *)location; *p; p++)
        if (*p <= 32 || *p == 127 || *p == '\\') return false;
    size_t n = strlen(origin);
    if (strncasecmp(origin, location, n) == 0 &&
        (location[n] == '/' || location[n] == '?' || location[n] == '#' || !location[n]))
        return true;
    // Relative paths cannot introduce a scheme or a new authority.
    if (location[0] == '/' && location[1] == '/') return false;
    size_t first = strcspn(location, "/?#");
    return memchr(location, ':', first) == NULL;
}
