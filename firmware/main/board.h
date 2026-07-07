// board.h — PicPak (ESP32-C3) hardware pin map & panel constants
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// ── E-paper panel SPI ──
#define PIN_EPD_SCLK 6
#define PIN_EPD_MOSI 3
#define PIN_EPD_MISO 4   // panel-ID readback
#define PIN_EPD_CS   9
#define PIN_EPD_DC   8
#define PIN_EPD_RST  10
#define PIN_EPD_BUSY 20

// ── User button (active-low) + battery ADC muxed on same pin ──
#define PIN_BTN      2
// Battery sense shares the button pin (GPIO2 = ADC1 channel 2). Reading it
// means momentarily driving the pin as ADC.
#define BATT_ADC_CHANNEL   2       /* ADC1_CHANNEL_2 == GPIO2 */
#define BATT_DIVIDER       1.45f   /* voltageMv = calibrated pin_mv * BATT_DIVIDER. Value is the
                                    * stock-firmware disassembly ratio (see docs/low-battery.md);
                                    * a full cell (pin_mv ~2897 @ 4.2V) reads 4.2V on our 12dB ADC.
                                    * For exactness: BATT_DIVIDER = V_battery(mV) / pin_mv. */

// ── Panel geometry (400×300, 4-colour BWRY, 2 bits/pixel) ──
#define EPD_W        400
#define EPD_H        300
#define EPD_FB_BYTES 30000   // 400*300*2/8
#define EPD_SPI_HZ   1000000 // 1 MHz
