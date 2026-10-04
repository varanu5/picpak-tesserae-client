// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"

static bool fail_alloc;
static size_t allocated, peak;
static struct { void *ptr; size_t size; } allocations[8];
static void *checked_malloc(size_t size) {
    if (fail_alloc) { fail_alloc = false; return NULL; }
    void *p = malloc(size); assert(p);
    for (size_t i=0; i<8; i++) if (!allocations[i].ptr) {
        allocations[i].ptr=p; allocations[i].size=size; allocated+=size;
        if (allocated>peak) peak=allocated;
        return p;
    }
    assert(false); return NULL;
}
static void checked_free(void *p) {
    if (!p) return;
    for (size_t i=0; i<8; i++) if (allocations[i].ptr==p) {
        allocated-=allocations[i].size; allocations[i].ptr=NULL; free(p); return;
    }
    assert(false);
}
#define malloc checked_malloc
#define free checked_free
#include "log_capture.c"
#undef malloc
#undef free

static esp_reset_reason_t reason = ESP_RST_POWERON;
static esp_sleep_wakeup_cause_t wake_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static vprintf_like_t hook;
static unsigned serial_lines, uploads;
static bool accept_upload;
static char received[7000];
static int serial(const char *fmt, va_list args) {
    assert(!s_lock); serial_lines++;
    char out[2048]; return vsnprintf(out,sizeof out,fmt,args);
}
vprintf_like_t esp_log_set_vprintf(vprintf_like_t cb) {
    vprintf_like_t old=hook ? hook : serial; hook=cb; return old;
}
int mock_log(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); int n=(hook ? hook : serial)(fmt,ap); va_end(ap); return n;
}
esp_reset_reason_t esp_reset_reason(void) { return reason; }
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) { return wake_cause; }
uint32_t esp_random(void) { return 123456; }
static bool upload(const char *body,size_t len,void *context) {
    assert(context==(void *)1 && !s_lock && len<sizeof received);
    assert(body[len]==0 && strlen(body)==len); uploads++;
    memcpy(received,body,len+1);
    mock_log("during-upload-line\n");
    return accept_upload;
}
static void boot(esp_reset_reason_t next) {
    checked_free(s_carry); s_carry=NULL; s_carry_len=0; s_carry_from=0;
    s_ready=s_busy=false; s_skipped=0; hook=NULL; s_previous=NULL;
    reason=next; wake_cause=next==ESP_RST_DEEPSLEEP ? ESP_SLEEP_WAKEUP_TIMER : ESP_SLEEP_WAKEUP_UNDEFINED;
    log_capture_init();
}
static void clean_case(const char *input,const char *forbidden) {
    char out[1024]; lr_clean(input,strlen(input),out,sizeof out);
    assert(!strstr(out,forbidden)); assert(strstr(out,"<redacted>"));
}
int main(void) {
    clean_case("url https://alice:pencil@host/a?secret=foo\n","pencil");
    clean_case("mqtts://alice:pencil@host:8883/a\n","pencil");
    clean_case("mqtt://alice:long-password\n","long-password");
    clean_case("GET https://relay/v1/pair/pair-code\n","pair-code");
    clean_case("GET /v1/pair/pair-code\n","pair-code");
    clean_case("tesserae://setup?v=2&key=my-secret\n","my-secret");
    clean_case("Bearer\tsecret-value\n","secret-value");
    clean_case("X-Tesserae-Token: secret-value\n","secret-value");
    clean_case("{\"password\":\"a spaced password\"}","spaced");
    clean_case("password=an unquoted spaced password\n","spaced");
    clean_case("{\"password\":\"escaped\\\"secret\"}","secret");
    boot(ESP_RST_POWERON);
    assert(sizeof s_ring==3072 && s_carry_len==0);
    char hb[768]="{\"fw_version\":\"test\"}"; uint32_t id=0;
    assert(!log_capture_status(hb,sizeof hb,&id));
    assert(strstr(hb,"\"logs\":{\"schema\":1,\"ring_bytes\":3072}"));
    assert(!strstr(hb,"diag"));
    mock_log("previous-paint-line https://host/image?key=secret-value Bearer private-token\n");
    unsigned before=serial_lines;
    s_busy=true; mock_log("concurrent line\n"); s_busy=false;
    mock_log("after concurrent line\n"); assert(serial_lines==before+2);
    char huge[600]; memset(huge,'x',sizeof huge); huge[sizeof huge-1]=0;
    mock_log("%s password=not-for-upload\n",huge);
    log_capture_paint_error(DIAG_PAINT_REFRESH_TIMEOUT);
    boot(ESP_RST_PANIC);
    assert(s_carry_len>0 && s_carry_len<=3072);
    strcpy(hb,"{}"); assert(log_capture_status(hb,sizeof hb,&id));
    assert(strstr(hb,"refresh_timeout") && strstr(hb,"panic") && !strncmp(hb,"{\"logs\"",7));
    log_capture_ack_report(id+1); assert(diag_get(&s_diag,NULL));
    char tiny[8]="{}"; assert(!log_capture_status(tiny,sizeof tiny,&id)); assert(!strcmp(tiny,"{}"));
    uint32_t old=s_rtc.uploaded;
    accept_upload=false;
    assert(!log_capture_upload(upload,(void *)1)); assert(s_rtc.uploaded==old);
    assert(strstr(received,"previous-paint-line") && strstr(received,"lines not captured"));
    assert(strstr(received,"oversized log line omitted"));
    assert(!strstr(received,"secret-value") && !strstr(received,"private-token"));
    assert(!strstr(received,"not-for-upload"));
    unsigned sent=uploads; fail_alloc=true;
    assert(!log_capture_upload(upload,(void *)1)); assert(uploads==sent && s_rtc.uploaded==old);
    accept_upload=true;
    assert(log_capture_upload(upload,(void *)1)); assert(s_rtc.uploaded!=old && !s_carry);
    assert(log_capture_upload(upload,(void *)1));
    assert(!strstr(received,"previous-paint-line") && strstr(received,"during-upload-line"));
    log_capture_ack_report(id); assert(!diag_get(&s_diag,NULL));
    log_capture_paint_error(DIAG_PAINT_DISPLAY_FAILED);
    boot(ESP_RST_SW); assert(diag_get(&s_diag,NULL));
    boot(ESP_RST_POWERON); assert(!diag_get(&s_diag,NULL) && !s_carry);
    for(int i=0;i<100;i++) mock_log("filling the retained log ring %03d with some data\n",i);
    boot(ESP_RST_DEEPSLEEP);
    for(int i=0;i<100;i++) mock_log("current wake overwrites early messages %03d\n",i);
    assert(log_capture_upload(upload,(void *)1));
    assert(strstr(received,"bytes dropped") && strlen(received)<6400);
    assert(peak<=9361);
    boot(ESP_RST_POWERON);
    assert(allocated==0);
    printf("Capture, retention, redaction, failure reporting and upload tests passed (peak heap %zu bytes)\n",peak);
}
