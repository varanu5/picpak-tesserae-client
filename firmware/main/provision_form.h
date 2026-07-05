// provision_form.h — pure (ESP-free) form parsing/validation for the portal.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef enum { PROVFORM_URL_OK, PROVFORM_URL_EMPTY, PROVFORM_URL_BADSCHEME } provform_url_result_t;

void provform_url_decode(char *s);
void provform_html_escape(const char *src, char *dst, size_t dst_sz);
bool provform_field(const char *body, const char *key, char *dst, size_t dst_sz);
provform_url_result_t provform_normalize_server_url(char *url, size_t url_sz);
