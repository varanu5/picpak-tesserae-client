// image_fetcher.h — download a frame URL into a caller buffer.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>

// GET `url`, streaming the body into `buf` (max `buf_sz`). Returns the number
// of bytes read on HTTP 200, or -1 on any error / non-200.
int image_fetch(const char *url, uint8_t *buf, size_t buf_sz);
