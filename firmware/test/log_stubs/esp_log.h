#pragma once
#include <stdarg.h>
typedef int (*vprintf_like_t)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t callback);
int mock_log(const char *fmt, ...);
#define ESP_LOGI(tag,fmt,...) mock_log("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag,fmt,...) mock_log("W %s: " fmt "\n", tag, ##__VA_ARGS__)
