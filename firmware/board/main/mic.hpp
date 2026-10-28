// The external I2S microphone on the development board's header.
//
// Plan, pin map and the INMP441 traps: firmware/docs/external_mic.md. The board
// it is wired to has no microphone of its own, so this is the only way F2's
// capture half runs before the badge arrives.
//
// This file is the bench end of it: a probe that answers one question, "is the
// microphone alive and hearing", in terms a multimeter cannot. Capture into the
// ring buffer comes after the probe says yes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace halo::board::mic {

// Starts the I2S receiver. False if the driver would not configure, which is a
// firmware problem, never a wiring one.
bool start();
void stop();

// What a window of samples looked like. The point of each field is to name a
// different failure instead of reporting "no sound".
struct Reading {
    bool ok = false;         // the read itself succeeded
    size_t frames = 0;
    int32_t raw_min = 0;     // the 32 bit words as they came off the wire
    int32_t raw_max = 0;
    size_t distinct = 0;     // how many of the words differed from the one before
    int16_t peak = 0;        // after the 24-in-32 shift, as 16 bit
    float rms_dbfs = -120.f;
    int slot = 0;            // which slot configuration produced this
};

// ---------------------------------------------------------------- capture

// Where captured samples go, called from the microphone's own task. It must do
// nothing but copy: no encoding, no radio, no display.
using SampleSink = void (*)(const int16_t* samples, size_t count);

// Starts a task that reads the microphone continuously and hands 16 bit samples
// to `sink` until capture_stop(). A task of its own on purpose: the main loop
// stops for about half a second whenever the panel refreshes, and the I2S DMA
// ring holds about 90 ms, so capturing from the main loop would lose speech
// every time a screen was drawn.
bool capture_start(SampleSink sink);
void capture_stop();
bool capturing();

// Reads `frames` samples and measures them. Blocks for frames/16000 seconds.
Reading read(size_t frames);

// Prints a few seconds of readings to the console, one line each, so tapping the
// microphone shows a number move. Says what each outcome means rather than
// leaving a wall of figures.
void probe(uint32_t seconds);

// Proves the I2S framing is really running, using a jumper from IO21 to IO19.
//
// Looping the bit clock back into the data pin is not enough on its own: every
// sample lands at the same phase of the clock it is sampling, so a live clock
// and a pin stuck high both read as constant all-ones. This swaps the roles of
// the two clock pins for the test, so the jumper carries the word select, which
// changes once a frame instead of once a bit. A line that alternates is a line
// that is being driven by something that is running.
void loopback_check();

// Reads the three microphone pins as plain inputs, once with the internal pull
// up and once with the pull down, with I2S stopped. A pin nothing is connected
// to follows whichever pull is on. A pin something is holding reads the same
// both times, and that is the difference between "not wired" and "wired to
// something that is not talking".
void line_check();

// Drives one microphone pin at a time high, for long enough to find it with a
// multimeter. Elecrow publishes which GPIOs reach the 2x10 header but not which
// physical pin each one is, and guessing it from a photograph is how a morning
// disappears. Leaves every pin back as a high impedance input.
void identify_pins();

}  // namespace halo::board::mic
