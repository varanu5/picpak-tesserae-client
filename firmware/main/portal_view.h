// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef bool (*portal_send_fn)(const char *, size_t, void *);
typedef const char *(*portal_value_fn)(const char *, void *);
bool portal_render(const char *html, portal_value_fn value, portal_send_fn send, void *context);
