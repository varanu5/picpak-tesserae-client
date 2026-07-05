// heartbeat.c — build the Tesserae status JSON.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "heartbeat.h"
#include "defaults.h"
#include "board.h"
#include "power.h"
#include "wifi_manager.h"

#include <stdio.h>

static const char *wake_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "timer";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_USB:       return "usb";
        default:                return "unknown";
    }
}

void heartbeat_json(char *dst, size_t dst_sz, int sleep_interval_s,
                    esp_reset_reason_t reset_reason) {
    if (!dst || dst_sz == 0) return;
    int mv = power_battery_mv();
    int pct = power_battery_pct(mv);   // reuse the one reading (single ADC read + log)
    int rssi = wifi_rssi();
    char ip[16] = {0};
    wifi_get_sta_ip(ip, sizeof(ip));

    snprintf(dst, dst_sz,
        "{\"battery_mv\":%d,\"battery_pct\":%d,\"rssi\":%d,\"ip\":\"%s\","
        "\"fw_version\":\"%s\",\"kind\":\"%s\",\"panel_w\":%d,\"panel_h\":%d,"
        "\"sleep_interval_s\":%d,\"next_sleep_s\":%d,\"wake_reason\":\"%s\"}",
        mv, pct, rssi, ip, FW_VERSION, DEVICE_KIND, EPD_W, EPD_H,
        sleep_interval_s, sleep_interval_s, wake_reason_str(reset_reason));
}
