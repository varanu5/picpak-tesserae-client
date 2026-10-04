#pragma once
static inline void test_log(const char *tag, const char *fmt, ...) {
    (void)tag;
    (void)fmt;
}
#define ESP_LOGI test_log
#define ESP_LOGW test_log
#define ESP_LOGE test_log
