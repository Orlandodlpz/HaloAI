# An external microphone on the development board

**Status:** agreed by the team 2026-10-20 as insurance, not as the plan. Not started.

The CrowPanel has no microphone, so F2's capture half has never run and **a real spoken
command has never been tested anywhere**. If the Waveshare badge does not arrive, the
team's written fallback for I2 and the video is the phone's microphone. This is a better
fallback than that one, and the reason is the demo: with a microphone on the device the
whole premise still works on camera — hold the button, speak to the device, read it on
e-ink, confirm with a press. With the phone's microphone you hold the device's button
while talking at a phone, which does not make sense to someone watching.

## Why it is worth doing even if the badge arrives

The untested risk is not microphone hardware. It is the capture pipeline with live
speech: a ring buffer under real timing, ADPCM of actual speech rather than a
pre-encoded clip, and the release-to-final-marker path when audio is still arriving.
None of that has been exercised. The flash clip proves the radio, not the capture.

So this is a rehearsal. If the badge lands late, the capture code is already debugged
and only the codec layer changes.

**What ports to the badge:** the ring buffer, backpressure, the 2 s window, chunk
pacing, the release path, and everything already in `core/src/audio.cpp`.
**What does not:** the driver itself. An INMP441 is a direct I2S MEMS microphone; the
badge uses an ES8311 codec on the PMU's rail, driven through Waveshare's examples.

## Hardware

The 2x10 header breaks out **IO3, IO9, IO15, IO17, IO19 and IO21**. An I2S microphone
needs three.

| Signal | Pin | Note |
| --- | --- | --- |
| BCLK (SCK) | IO21 | |
| WS (LRCL) | IO17 | |
| DOUT (SD) | IO19 | |
| L/R | GND | left channel, which is what the driver should read |

**Corrected 2026-10-27: BCLK was IO15 in the first version of this table, and IO15 is
`kBtnSide`,** the breadboard side button the F0 gesture timings were measured on. Sharing
it would have broken both. The clock is IO21; IO9 is the one header pin still free.

**Do not use IO3.** It is an ESP32-S3 strapping pin and anything holding it at reset
stops the board booting, which presents as a dead board rather than a wiring mistake.

The panel is on SPI2 (IO11, IO12, IO45, IO46, IO47, IO48) and the microSD on its own
bus (IO40, IO13, IO39, IO10), so none of this collides.

## Firmware

ESP-IDF v5.5.5 has the new I2S API, `driver/i2s_std.h`. Target the contract, not the
part: **16 kHz, mono, 16 bit** is what everything downstream expects.

Two things cost people an evening on the INMP441 specifically:

- It sends **24 bits inside a 32 bit slot**, MSB aligned. Read 32 bit samples and shift
  down to get 16 bit, rather than trusting a 16 bit slot configuration to do it.
- With L/R tied to ground it speaks only in the **left** slot; the right slot is zeros.
  A capture that is half silence usually means the wrong slot is being read, not a dead
  microphone.

Where it plugs in:

- `Action::StartCapture` and `Action::StopCapture` already come out of the state machine
  and are no-ops on the board today (`board/main/main.cpp`). That is the hook.
- `core/include/halo/audio.hpp` already has `AdpcmEncoder`, `build_chunk` and
  `build_final_marker`, all host tested, and `kSamplesPerChunk` is 320 (20 ms).
- `AudioTx` in `main.cpp` already does windowing, acknowledgments, `resend_from` and the
  300 ms final marker resend. It currently sources from a clip in flash. The job is a
  second source, not a second streamer.
- The ring buffer is 2 s, 100 chunks, about 17.6 KB (BRIEFING section 2, provisional
  until measured). If the link stalls past it, capture stops with FAILURE. Audio is
  never silently dropped.

## When this is worth stopping

**Time-box it to one evening.** If it is not capturing audible speech by the end of it,
stop and keep the phone's microphone as the fallback. I2S clock and DMA configuration is
a known sinkhole and there are F3 items still open that do not need any hardware.

## What it does not fix

It closes no phase gate: every number from it is a CrowPanel number. It gives no F4 at
all — no power, no battery life, no fuel gauge — and says nothing about panel legibility
at the badge's 235 DPI or whether the thing fits a credit card. A working microphone here
must not make the badge feel less urgent.

## Before writing any code

Confirm which microphone is actually in hand. The INMP441 is listed in the materials
inventory under "removed or replaced", because the badge has one built in, which reads
like it was dropped from the plan rather than bought. If one has to be ordered it goes on
the same order as the flat LiPo, the side buttons and the vibration motors, today: lead
time is the thing this whole plan exists to defend against.
