// Pin maps for both boards, selected by the same build flag that selects the
// display backend:
//   idf.py -DHALO_PANEL=crowpanel build   (the Elecrow development board)
//   idf.py -DHALO_PANEL=waveshare build   (the badge, the actual product)
//
// The two boards share almost nothing. The badge's panel sits on IO9/IO10/IO3,
// which on the development board are a spare, the microSD chip select and a
// strapping pin; the badge's rotary up and down are swapped relative to the
// development board; and the badge has no GPIO for panel power at all.
#pragma once

#include "driver/gpio.h"

namespace halo::board {

// A button this board does not have. configure_input() skips it and pressed()
// always reads false, so the input loop needs no conditionals.
constexpr gpio_num_t kBtnNone = GPIO_NUM_NC;

#if defined(HALO_PANEL_WAVESHARE)

// ===================== Waveshare ESP32-S3 e-Paper 3.97 (the badge) =========
//
// Every constant below is from Waveshare's own ESP-IDF examples
// (github.com/waveshareteam/ESP32-S3-ePaper-3.97), read at the source rather
// than copied from the product page:
//   panel    ESP-IDF/01_E-Paper_Example/components/epaper_port/epaper_port.h
//   buttons  ESP-IDF/08_.../components/button_bsp/button_bsp.c
//   I2C      ESP-IDF/08_.../components/i2c_bsp/i2c_bsp.h
//   codec    ESP-IDF/02_Mic_test/components/codec_board/board_cfg.txt
//            (entry "S3_ePaper_3_97")

// ---- Panel. SPI3 at 20 MHz. BUSY is active HIGH (1 means busy). ----
constexpr gpio_num_t kPanelClk = GPIO_NUM_11;
constexpr gpio_num_t kPanelMosi = GPIO_NUM_12;
constexpr gpio_num_t kPanelCs = GPIO_NUM_10;
constexpr gpio_num_t kPanelDc = GPIO_NUM_9;
constexpr gpio_num_t kPanelRes = GPIO_NUM_46;
constexpr gpio_num_t kPanelBusy = GPIO_NUM_3;

// There is NO GPIO panel power enable on this board. The panel rail is the
// PMU's ALDO3, over I2C. The development board's IO7 trap does not exist here;
// a different one replaces it, in the PMU. See display_waveshare.cpp.
constexpr gpio_num_t kPanelPower = GPIO_NUM_NC;

// ---- I2C: the PMU, the RTC, the temperature sensor, the IMU and the audio
// codec all share this bus. 400 kHz. ----
constexpr gpio_num_t kI2cSda = GPIO_NUM_41;
constexpr gpio_num_t kI2cScl = GPIO_NUM_42;
constexpr uint8_t kAxp2101Addr = 0x34;   // the PMU, the board calls it TG28
constexpr uint8_t kEs8311Addr = 0x18;    // the microphone and speaker codec

// ---- Inputs. Active low, internal pull ups (the vendor enables them; there
// are no external ones documented). ----
//
// UP AND DOWN ARE SWAPPED relative to the development board: there up is IO6
// and down is IO4. Getting this backwards moves the highlight the wrong way
// and is easy to miss, because everything still "works".
constexpr gpio_num_t kKnobUp = GPIO_NUM_4;
constexpr gpio_num_t kKnobPress = GPIO_NUM_5;   // wake, never a gesture
constexpr gpio_num_t kKnobDown = GPIO_NUM_6;

// The side button the badge ships with is still Planned and not soldered, so
// BOOT stands in for it: it is the only free momentary button on the board and
// it drives the whole gesture path (hold to record, press to confirm, double
// press to cancel). When the real side button arrives this is the one line
// that changes.
//
// BOOT is IO0, a strapping pin. Holding it through a reset puts the chip in the
// ROM loader instead of booting, so a hold started before the board is up looks
// like a dead board. Press it after boot, not through one.
constexpr gpio_num_t kBtnSide = GPIO_NUM_0;

// The badge has no MENU or EXIT button. The PWR button is NOT on a GPIO at all:
// it is the PMU's PWRKEY, with its on and off hold times set over I2C, so
// firmware cannot read it as an input.
constexpr gpio_num_t kBtnMenu = kBtnNone;
constexpr gpio_num_t kBtnExit = kBtnNone;

// ---- Audio: an ES8311 codec on the shared I2C bus, not a bare I2S
// microphone. Recorded here so the F2 port has the map; mic.cpp still drives
// an INMP441 and does not speak to a codec yet, so it refuses to start on this
// board rather than capturing noise.
constexpr gpio_num_t kMicMclk = GPIO_NUM_13;
constexpr gpio_num_t kMicBclk = GPIO_NUM_14;
constexpr gpio_num_t kMicWs = GPIO_NUM_47;
constexpr gpio_num_t kMicData = GPIO_NUM_21;    // DIN, the codec's output to us
constexpr gpio_num_t kSpkData = GPIO_NUM_48;    // DOUT, unused: no speaker
constexpr gpio_num_t kSpkEnable = GPIO_NUM_39;  // amplifier enable, unused

#else  // HALO_PANEL_CROWPANEL

// ===================== Elecrow CrowPanel 5.79 (development board only) =====
//
// Settled 2026-10-13 from Elecrow's factory source and the Eagle schematic, and
// confirmed by two independent third party ports. The two "conflicting" sources
// that were in docs/CLAUDE.md turned out to describe different peripherals: the
// panel and the microSD card are separate buses with separate power switches.

// ---- Panel, bit banged by Elecrow, hardware SPI for us via the GPIO matrix ----
constexpr gpio_num_t kPanelMosi = GPIO_NUM_11;
constexpr gpio_num_t kPanelClk = GPIO_NUM_12;
constexpr gpio_num_t kPanelCs = GPIO_NUM_45;   // also an ESP32-S3 strapping pin
constexpr gpio_num_t kPanelDc = GPIO_NUM_46;   // also an ESP32-S3 strapping pin
constexpr gpio_num_t kPanelRes = GPIO_NUM_47;
constexpr gpio_num_t kPanelBusy = GPIO_NUM_48;

// Must be HIGH or the panel is dead. It gates the panel's ground return through
// a 2N7002 MOSFET and is off at reset. This is the first thing to check when the
// screen does nothing.
constexpr gpio_num_t kPanelPower = GPIO_NUM_7;

// ---- microSD, a separate bus with its own power switch. Unused so far. ----
constexpr gpio_num_t kSdMosi = GPIO_NUM_40;
constexpr gpio_num_t kSdMiso = GPIO_NUM_13;
constexpr gpio_num_t kSdClk = GPIO_NUM_39;
constexpr gpio_num_t kSdCs = GPIO_NUM_10;
constexpr gpio_num_t kSdPower = GPIO_NUM_42;

// ---- Inputs. Active LOW, external 4.7 K pull ups (schematic R27 to R31), so
// no internal pull is configured. ----
constexpr gpio_num_t kBtnMenu = GPIO_NUM_2;   // stands in for the badge's side button
constexpr gpio_num_t kBtnExit = GPIO_NUM_1;   // spare, logged only for now
// Careful: the badge swaps these. Waveshare has up on IO4 and down on IO6, the
// CrowPanel has up on IO6 and down on IO4. Getting it backwards makes the
// highlight move the wrong way and is easy to miss.
constexpr gpio_num_t kKnobUp = GPIO_NUM_6;
constexpr gpio_num_t kKnobDown = GPIO_NUM_4;
constexpr gpio_num_t kKnobPress = GPIO_NUM_5;  // wake, never a gesture

// The external side button on the breadboard, on a free header pin. This is the
// part the badge will actually use, so the real debounce, hold and double press
// timings have to be measured on it, not on the onboard MENU button.
// IO3 is deliberately avoided: it is a strapping pin and is on the same header.
constexpr gpio_num_t kBtnSide = GPIO_NUM_15;

// ---- The external I2S microphone on the same 2x10 header. ----
//
// The header breaks out IO3, IO9, IO15, IO17, IO19 and IO21. IO3 is a strapping
// pin and anything holding it at reset stops the board booting, which presents
// as a dead board rather than a wiring mistake. IO15 is the side button above.
// That leaves these three, and IO9 spare.
//
// firmware/docs/external_mic.md had BCLK on IO15, which is the side button. The
// clock is on IO21 instead; the doc is corrected.
constexpr gpio_num_t kMicBclk = GPIO_NUM_21;   // SCK on an INMP441
constexpr gpio_num_t kMicWs = GPIO_NUM_17;     // WS / LRCL
constexpr gpio_num_t kMicData = GPIO_NUM_19;   // SD, the microphone's output
// L/R goes to GND: it then speaks in the left slot, which is what the driver
// reads. Floating is not a third option; it has to be tied.

#endif

}  // namespace halo::board
