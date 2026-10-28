// Pin map for the Elecrow CrowPanel ESP32 E-Paper 5.79 (development board only).
//
// Settled 2026-10-13 from Elecrow's factory source and the Eagle schematic, and
// confirmed by two independent third party ports. The two "conflicting" sources
// that were in docs/CLAUDE.md turned out to describe different peripherals: the
// panel and the microSD card are separate buses with separate power switches.
//
// The badge (Waveshare ESP32-S3 e-Paper 3.97) has a different map. When it
// arrives this file gains a second block behind the same build flag that selects
// the display backend.
#pragma once

#include "driver/gpio.h"

namespace halo::board {

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

}  // namespace halo::board
