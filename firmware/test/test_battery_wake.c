// SPDX-License-Identifier: AGPL-3.0-or-later
// Wake sequence tests using the actual main, splash and battery decision code.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <setjmp.h>
#include "defaults.h"
#include "lowbatt.h"
#include "manual_core.h"
#include "button_event.h"
#include "connection_retry.h"
#include "setup_check.h"
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define EPD_W 400
#define EPD_H 300
#define EPD_FB_BYTES 30000
#define SETUP_SSID_CAPACITY 33
#define ESP_ERR_INVALID_SIZE -2
#define ESP_OK 0
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define pdMS_TO_TICKS(x) (x)
typedef int esp_err_t;
typedef int esp_reset_reason_t;
enum { ESP_RST_POWERON, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_INT_WDT, ESP_RST_WDT, ESP_RST_TASK_WDT, ESP_RST_SW };
enum { ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_GPIO };
enum { RELAY_PAIR_DONE, RELAY_PAIR_WAITING, RELAY_PAIR_EXPIRED };
static void quiet_log(const char *tag, const char *fmt, ...) {(void)tag;(void)fmt;}
#define ESP_LOGI quiet_log
#define ESP_LOGW quiet_log
#define ESP_LOGE quiet_log
static jmp_buf asleep;
static int reason, wake_cause, mv, mode, screen, wifi_calls, paints, low_paints, clears, ble_calls;
static uint32_t slept, saved_sleep;
static bool manual, cached, splash_fails, net_ok, scheduled, creds;
static bool battery_screen, store_fails, frame_fails, ready_fails, restart_during_paint;
static btn_gesture_t gesture;
static uint8_t frame[30000];
static int64_t ticks;
static esp_reset_reason_t esp_reset_reason(void) {return reason;}
static int esp_sleep_get_wakeup_cause(void) {return wake_cause;}
static void log_capture_init(void) {}
static int config_init(void) {return 0;}
static void led_init(void) {}
static void led_ack(void) {}
static void wake_align_begin(bool a,bool b) {(void)a;(void)b;}
static btn_gesture_t power_boot_gesture(void) {return gesture;}
static bool config_take_ble_recovery(void) {return false;}
static void power_measure_battery(void) {}
static int power_battery_mv(void) {return mv;}
static bool config_screen_is_bluetooth(void) {return manual;}
static bool config_lowbatt_screen_pending(void) {return battery_screen;}
static int config_set_lowbatt_screen(bool p) {if(store_fails)return -1;battery_screen=p;return 0;}
static const uint8_t _binary_splash_lowbatt_bin_start[30000] = {1};
static const uint8_t _binary_splash_setup_bin_start[30000] = {3};
static const uint8_t _binary_splash_paired_bin_start[30000] = {4};
static const uint8_t _binary_splash_revoked_bin_start[30000] = {5};
#define _binary_splash_lowbatt_bin_end (_binary_splash_lowbatt_bin_start + 30000)
#define _binary_splash_setup_bin_end (_binary_splash_setup_bin_start + 30000)
#define _binary_splash_paired_bin_end (_binary_splash_paired_bin_start + 30000)
#define _binary_splash_revoked_bin_end (_binary_splash_revoked_bin_start + 30000)
static int setup_ap_ssid(char *s) {strcpy(s,"Tesserae-Setup-1234");return 0;}
static void setup_screen_network(uint8_t*f,const char*s) {(void)f;(void)s;}
static bool setup_screen_wifi_qr(uint8_t*f,const char*s,const char*p) {(void)f;(void)s;(void)p;return true;}
static void power_deep_sleep(uint32_t s) {slept=s;longjmp(asleep,1);}
static void power_sleep_until_button(void) {power_deep_sleep(0);}
static void power_scheduled_sleep(uint32_t s) {scheduled=true;power_deep_sleep(s);}
static void ble_setup_run(int s) {(void)s;ble_calls++;}
static void ble_photo_run(int s) {(void)s;ble_calls++;}
static void esp_restart(void) {longjmp(asleep,2);}
static void maintenance_screen_photo_ready(uint8_t *f) {f[0]=2;}
static uint8_t *framebuf(void) {return frame;}
static int epd_present(const uint8_t *f) {
 if(f==_binary_splash_lowbatt_bin_start) {
  low_paints++;
  assert(battery_screen && !cached);
  if(restart_during_paint)longjmp(asleep,2);
  if(splash_fails)return -1;
 } else {
  paints++;
  if((f[0]==2 && ready_fails) || (f[0]==0 && frame_fails))return -1;
 }
 screen=f[0];return 0;
}
static bool config_get_wifi(char *s,size_t n,char*p,size_t m) {(void)s;(void)n;(void)p;(void)m;return creds;}
static void config_clear_frame_ref(void) {cached=false;clears++;}
static int provisioning_run_blocking(const char *s) {(void)s;return -1;}
static int config_save_screen_mode(bool a,void*b) {(void)a;(void)b;return 0;}
static bool config_take_paired_pending(void) {return false;}
static uint8_t config_get_transport(uint8_t fallback) {(void)fallback;return mode==0?0:1;}
static bool config_relay_configured(void) {return mode==2;}
static uint32_t config_get_sleep_s(uint32_t fallback) {(void)fallback;return saved_sleep;}
static void vTaskDelay(int t) {ticks+=(int64_t)t*1000;}
static int wifi_start_sta(void) {wifi_calls++;return net_ok?0:-1;}
static void wifi_stop(void) {}
static void config_get_relay_url(char*s,size_t n) {snprintf(s,n,"http://relay.test");}
static void config_get_server_url(char*s,size_t n) {snprintf(s,n,"http://server.test");}
static void wifi_sync_ntp(void) {}
static bool relay_pairing_pending(void) {return false;}
static int relay_pair_step(void) {return RELAY_PAIR_DONE;}
static bool relay_ready(void) {return true;}
static uint32_t esp_random(void) {return 12345;}
static bool relay_pairing_revoked(void) {return false;}
static bool relay_poll_frame(void) {return !cached;}
static int64_t esp_timer_get_time(void) {return ticks;}
static bool relay_connection_ok(void) {return net_ok;}
static bool rest_connection_ok(void) {return net_ok;}
static bool mqtt_connection_ok(void) {return net_ok;}
static void relay_end_wake(void) {}
static void relay_forget_revoked_pairing(void) {}
static int relay_run_loop(const char*b,uint32_t e) {(void)b;(void)e;return saved_sleep;}
static int rest_run_loop(int r,const char*b,uint32_t e) {(void)r;(void)b;(void)e;return saved_sleep;}
static int mqtt_run_loop(int r) {(void)r;return saved_sleep;}
static bool wifi_fail_looks_like_bad_password(void) {return false;}
static bool wifi_fail_looks_like_no_ap(void) {return false;}
static const uint8_t *pending(void) {frame[0]=0;return net_ok&&!cached?frame:NULL;}
static const uint8_t *relay_pending_frame(void) {return pending();}
static const uint8_t *rest_pending_frame(void) {return pending();}
static const uint8_t *mqtt_pending_frame(void) {return pending();}
static void relay_frame_painted(void) {cached=true;}
static void rest_frame_painted(void) {cached=true;}
static void mqtt_frame_painted(void) {cached=true;}
static const char *esp_err_to_name(int e) {(void)e;return "test error";}
#define TAG splash_tag
#include "actual_splash.c"
#undef TAG
#include "actual_lowbatt.c"
#include "actual_main.c"
static void fresh(int transport) {
 s_lb=(lowbatt_state_t){0};s_connection_retry=(connection_retry_t){0};
 s_lowbatt_splash_painted=false;battery_screen=store_fails=frame_fails=ready_fails=restart_during_paint=false;
 mode=transport;manual=false;cached=true;screen=0;splash_fails=false;net_ok=true;creds=true;
 mv=3900;gesture=BTN_GESTURE_NONE;reason=ESP_RST_DEEPSLEEP;wake_cause=ESP_SLEEP_WAKEUP_TIMER;saved_sleep=60;
}
static void run(void) {
 wifi_calls=paints=low_paints=clears=ble_calls=0;scheduled=false;ticks=0;
 if(reason!=ESP_RST_DEEPSLEEP) {s_lb=(lowbatt_state_t){0};s_lowbatt_splash_painted=false;}
 if(setjmp(asleep)==0) {app_main();assert(false);}
}
static void lock(void) {
 mv=3390;run();assert(!s_lb.lock);
 mv=3380;run();assert(s_lb.lock&&screen==1&&low_paints==1&&slept==86400&&!wifi_calls);
}
int main(void) {
 for(int t=0;t<3;t++) {
  fresh(t);lock();
  assert(battery_screen && !cached);
  s_connection_retry.failures=3;
  gesture=BTN_GESTURE_TAP;wake_cause=ESP_SLEEP_WAKEUP_GPIO;run();
  assert(!wifi_calls&&!low_paints&&!paints&&slept==86400&&!scheduled);
  for(int invalid=-1;invalid<=2499;invalid+=500) {
   mv=invalid;run();assert(s_lb.lock&&!wifi_calls&&!paints&&!low_paints&&slept==86400);
  }
  mv=3560;gesture=BTN_GESTURE_NONE;run();
  assert(!s_lb.lock&&wifi_calls==1&&paints==1&&screen==0&&slept==60&&scheduled&&!battery_screen);
  fresh(t);lock();gesture=BTN_GESTURE_REFRESH;mv=3300;run();
  assert(!s_lb.lock&&paints==1&&screen==0&&!battery_screen);
  gesture=BTN_GESTURE_NONE;run();assert(s_lb.lock&&screen==1&&slept==86400&&!wifi_calls);
  fresh(t);lock();gesture=BTN_GESTURE_PROVISION;run();assert(slept==86400&&!wifi_calls&&!low_paints);
  fresh(t);lock();gesture=BTN_GESTURE_MAINTENANCE;run();assert(slept==86400&&!ble_calls&&!low_paints);
  fresh(t);mv=3390;run();splash_fails=true;mv=3380;run();
  assert(s_lb.lock&&low_paints==1&&screen==0&&slept==86400&&battery_screen);
  run();assert(low_paints==1&&!wifi_calls);
  mv=0;run();assert(!low_paints&&!wifi_calls&&slept==86400);
  mv=3380;splash_fails=false;run();assert(low_paints==1&&screen==1);
  run();assert(!low_paints&&!wifi_calls&&slept==86400);
  fresh(t);lock();reason=ESP_RST_POWERON;mv=3900;run();
  assert(!s_lb.lock&&cached&&paints==1&&screen==0&&!battery_screen);
  fresh(t);lock();reason=ESP_RST_SW;mv=3900;net_ok=false;run();
  assert(battery_screen&&screen==1&&!paints);
  reason=ESP_RST_DEEPSLEEP;net_ok=true;frame_fails=true;run();
  assert(battery_screen&&screen==1&&paints==1&&!cached);
  frame_fails=false;run();assert(!battery_screen&&screen==0&&cached);
  fresh(t);mv=3390;run();mv=3380;restart_during_paint=true;run();
  assert(battery_screen&&!cached);
  restart_during_paint=false;reason=ESP_RST_POWERON;mv=3900;run();
  assert(screen==0&&paints==1&&!battery_screen);
  fresh(t);mv=3390;run();mv=3380;store_fails=true;run();
  assert(s_lb.lock&&!low_paints&&!wifi_calls&&slept==86400);
  store_fails=false;run();assert(low_paints==1&&battery_screen&&screen==1);
 }
 puts("battery wake: REST, MQTT and relay recovery, invalid ADC, paint retries, reset and storage failure passed");
 fresh(1);manual=true;lock();run();assert(!ble_calls&&!low_paints&&slept==86400);
 mv=0;gesture=BTN_GESTURE_TAP;run();assert(s_lb.lock&&!ble_calls&&!paints&&screen==1);
 mv=3560;gesture=BTN_GESTURE_NONE;ready_fails=true;run();
 assert(screen==1&&!ble_calls&&battery_screen&&slept==86400);
 ready_fails=false;run();assert(screen==2&&!ble_calls&&!battery_screen);
 run();assert(!paints&&!ble_calls);
 fresh(1);manual=true;lock();reason=ESP_RST_POWERON;mv=3900;run();
 assert(screen==2&&!ble_calls&&!battery_screen);
 fresh(1);manual=true;lock();gesture=BTN_GESTURE_TAP;mv=3560;run();
 assert(screen==2&&ble_calls==1&&!battery_screen);
 fresh(1);manual=true;lock();gesture=BTN_GESTURE_REFRESH;mv=3380;run();
 assert(s_lb.lock&&!ble_calls&&slept==86400);
 fresh(1);manual=true;mv=3390;run();splash_fails=true;mv=3380;run();
 assert(s_lb.lock&&low_paints==1&&screen==0);
 splash_fails=false;run();assert(low_paints==1&&screen==1);
 run();assert(!low_paints&&!ble_calls);
 puts("battery wake: Bluetooth recovery, ready screen retry, restart and safe button checks passed");
 fresh(1);battery_screen=true;splash_show_setup();assert(!battery_screen&&screen==3);
 battery_screen=true;splash_show_paired();assert(!battery_screen&&screen==4);
 battery_screen=true;splash_show_revoked();assert(!battery_screen&&screen==5);
 fresh(1);reason=ESP_RST_BROWNOUT;mv=3200;run();assert(!s_lb.lock&&!low_paints&&slept==180);
 puts("battery wake: replacement splashes and existing brownout delay passed");
}
