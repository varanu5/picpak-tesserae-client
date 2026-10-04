// Display protocol and failure-path regression tests at a 100 Hz RTOS tick.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "epd_driver.h"
#include "log_capture.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"

typedef struct { uint8_t cmd, data[30000]; size_t n; int64_t at; } record_t;
static record_t records[128];
static size_t nrecords;
static int dc, mode, reset_low, reset_count, transfers, fail_at, timeout_cmd;
static int bus_init_error, add_error, gpio_error, bus_frees;
static bool fail_all, stuck, fault_busy, fault_used;
static int64_t now, busy_start, busy_end, next_command_at;
static uint8_t fb[EPD_FB_BYTES];
static diag_paint_t last_failure;
static unsigned failure_reports;
void log_capture_paint_error(diag_paint_t failure) { last_failure=failure; failure_reports++; }

static void clear_probe(void) {
    memset(records, 0, sizeof(records)); nrecords = 0;
    dc = reset_low = reset_count = transfers = 0;
    fail_at = timeout_cmd = -1;
    fail_all = stuck = fault_busy = fault_used = false;
    now = busy_start = busy_end = next_command_at = 0;
    bus_init_error = add_error = gpio_error = 0;
    last_failure=DIAG_PAINT_NONE; failure_reports=0;
}
int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(TickType_t t) { assert(t > 0); now += (int64_t)t * 10000; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "mock"; }
uint8_t config_get_waveform(uint8_t fallback) { (void)fallback; return (uint8_t)mode; }
int gpio_config(const gpio_config_t *c) { (void)c; return gpio_error; }
int gpio_set_level(int pin, int value) {
    if (pin == PIN_EPD_DC) dc = value;
    if (pin == PIN_EPD_RST) {
        if (!value) reset_low = 1;
        if (value && reset_low) {
            reset_low = 0; reset_count++;
            fault_busy = false; busy_start = busy_end = next_command_at = 0;
        }
    }
    return 0;
}
int gpio_get_level(int pin) {
    assert(pin == PIN_EPD_BUSY);
    return !(stuck || fault_busy || (now >= busy_start && now < busy_end));
}
int spi_bus_initialize(int h, const spi_bus_config_t *c, int dma) {
    assert(h == SPI2_HOST && dma == SPI_DMA_CH_AUTO);
    assert(c->mosi_io_num == 3 && c->miso_io_num == 4 && c->sclk_io_num == 6);
    assert(c->max_transfer_sz == 512);
    return bus_init_error;
}
int spi_bus_add_device(int h, const spi_device_interface_config_t *c, spi_device_handle_t *d) {
    assert(h == SPI2_HOST && c->clock_speed_hz == 1000000 && c->mode == 0);
    assert(c->spics_io_num == 9);
    if (add_error) return add_error;
    *d = (void *)1; return ESP_OK;
}
int spi_bus_free(int h) { assert(h == SPI2_HOST); bus_frees++; return ESP_OK; }
static void busy_for_operation(uint8_t cmd) {
    // Power/refresh BUSY asserts 1 ms later: an immediate HIGH sample is premature.
    busy_start = now + 1000; busy_end = now + 51000;
    next_command_at = busy_end + 100000;
    if (cmd == timeout_cmd && !fault_used) { fault_busy = fault_used = true; }
}
int spi_device_polling_transmit(spi_device_handle_t dev, spi_transaction_t *t) {
    assert(dev && t->length > 0 && t->length <= 4096);
    int attempt = transfers++;
    if (fail_all || attempt == fail_at) return ESP_FAIL;
    assert(!fault_busy && !stuck);
    if (!dc) {
        assert(t->length == 8 && nrecords < 128);
        if (now < next_command_at) fprintf(stderr,"early cmd=%02x mode=%d time=%lld required=%lld fail_at=%d timeout_cmd=%d\n",*(const uint8_t*)t->tx_buffer,mode,(long long)now,(long long)next_command_at,fail_at,timeout_cmd);
        assert(now >= next_command_at);
        record_t *r = &records[nrecords++];
        r->cmd = *(const uint8_t *)t->tx_buffer; r->at = now;
        if (r->cmd == 0x04) busy_for_operation(r->cmd);
    } else {
        assert(nrecords);
        record_t *r = &records[nrecords-1]; size_t n = (size_t)t->length / 8;
        assert(r->n + n <= sizeof(r->data));
        memcpy(r->data+r->n, t->tx_buffer, n); r->n += n;
        if (r->cmd == 0x12 || r->cmd == 0x02) busy_for_operation(r->cmd);
        if (mode != EPD_WAVE_NATIVE && (r->cmd == 0x00 || r->cmd == 0x61 || r->cmd == 0x65)) {
            busy_start = now; busy_end = now + 30000;
            next_command_at = busy_end + 100000;
            if (r->cmd == timeout_cmd && !fault_used) fault_busy = fault_used = true;
        }
    }
    return ESP_OK;
}
static size_t count_cmd(uint8_t c) {
    size_t n = 0; for (size_t i=0;i<nrecords;i++) n += records[i].cmd==c; return n;
}
static record_t *find_cmd(uint8_t c) {
    for (size_t i=0;i<nrecords;i++) if(records[i].cmd==c) return &records[i];
    assert(false); return NULL;
}
static void check_fixture(const char *dir, const char *name, const uint8_t *data, size_t len) {
    char path[1024]; snprintf(path,sizeof path,"%s/%s",dir,name);
    FILE *f=fopen(path,"rb"); assert(f);
    uint8_t expected[1024]; size_t n=fread(expected,1,sizeof expected,f); assert(!ferror(f)); fclose(f);
    assert(n==len && !memcmp(expected,data,n));
}
int main(int argc, char **argv) {
    assert(argc==2);
    for(size_t i=0;i<sizeof fb;i++) fb[i]=(uint8_t)i;
    clear_probe(); assert(epd_present(NULL)==ESP_ERR_INVALID_ARG && !transfers);
    assert(epd_display(fb)==ESP_ERR_INVALID_STATE);
    assert(epd_sleep()==ESP_ERR_INVALID_STATE);
    // Setup failures must return, not abort; adding a device must release a newly opened bus.
    gpio_error=ESP_FAIL; assert(epd_init()==ESP_FAIL);
    clear_probe(); bus_init_error=ESP_FAIL; assert(epd_init()==ESP_FAIL);
    clear_probe(); add_error=ESP_FAIL; assert(epd_init()==ESP_FAIL && bus_frees==1);
    size_t injected=0;
    for(mode=0;mode<3;mode++) {
        clear_probe(); assert(epd_present(fb)==ESP_OK);
        assert(failure_reports==0);
        int successful_transfers=transfers;
        assert(reset_count==1 && !count_cmd(0x70));
        record_t *r=find_cmd(0x10); assert(r->n==sizeof fb && !memcmp(r->data,fb,sizeof fb));
        r=find_cmd(0x12); assert(r->n==1 && r->data[0]==0);
        r=find_cmd(0x02); assert(r->n==1 && r->data[0]==0);
        r=find_cmd(0x07); assert(r->n==1 && r->data[0]==0xa5);
        assert(records[nrecords-1].cmd==0x07);
        if(mode!=EPD_WAVE_NATIVE) {
            r=find_cmd(0x20);
            check_fixture(argv[1],mode==0?"epd_lut_5s_wire.bin":"epd_lut_10s_wire.bin",r->data,r->n);
        } else {
            uint8_t table[256]; size_t n=0;
            for(size_t i=0;records[i].cmd!=0x10;i++) {
                r=&records[i]; table[n++]=r->cmd;table[n++]=(uint8_t)r->n;
                memcpy(table+n,r->data,r->n);n+=r->n;
            }
            check_fixture(argv[1],"epd_native_commands.bin",table,n);
        }
        // Every SPI failure in a successful cycle must remain a failure even if cleanup succeeds.
        for(int i=0;i<successful_transfers;i++) {
            clear_probe();fail_at=i;
            assert(epd_present(fb)==ESP_FAIL);
            assert(reset_count==2 && records[nrecords-1].cmd==0x07);
            assert(count_cmd(0x10)<=1); // cleanup never redraws
            injected++;
        }
        int busy_commands[]={0x04,0x12,0x02,0x00,0x61,0x65};
        for(size_t i=0;i<(mode==EPD_WAVE_NATIVE?3:6);i++) {
            clear_probe();timeout_cmd=busy_commands[i];
            assert(epd_present(fb)==ESP_ERR_TIMEOUT);
            assert(fault_used && reset_count==2 && now<52000000);
            assert(failure_reports==1);
            assert(last_failure==(timeout_cmd==0x12 ? DIAG_PAINT_REFRESH_TIMEOUT
                : (timeout_cmd==0x00 || timeout_cmd==0x61 || timeout_cmd==0x65)
                ? DIAG_PAINT_INIT_FAILED : DIAG_PAINT_READY_TIMEOUT));
            assert(records[nrecords-1].cmd==0x07 && count_cmd(0x10)<=1);
        }
    }
    mode=EPD_WAVE_5S;
    clear_probe(); stuck=true;
    assert(epd_present(fb)==ESP_ERR_TIMEOUT && transfers==0 && now<11000000);
    assert(failure_reports==1 && last_failure==DIAG_PAINT_INIT_FAILED);
    clear_probe(); fail_all=true;
    assert(epd_present(fb)==ESP_FAIL && transfers==2 && nrecords==0);
    clear_probe(); assert(epd_present(fb)==ESP_OK); // retry after a persistent fault is removed
    printf("epd: all 3 modes, golden payloads, delayed BUSY, 15 BUSY timeouts, %zu SPI failures, setup/recovery passed\n",injected);
    return 0;
}
