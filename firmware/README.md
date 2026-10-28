# HaloAI firmware (Victor, branch `Hardware`)

The device: hold the side button, speak, read the e-paper screen, confirm with a
single press. See `docs/BRIEFING.md` section 2 for the V1 state machine and the BLE
contract, and section 3.3 for the phases.

**State: F0 done, F1 done on both boards, F2 capture working on the development
board, F3 in progress.** Framework is ESP-IDF (v5.5.5). The state machine drives
real buttons, the e-paper renders real screens, and BLE streams audio to a phone.

**The badge (Waveshare ESP32-S3 e-Paper 3.97) is in hand and drawing its own
screens as of 2026-10-27.** One build flag picks the board:

```sh
idf.py -DHALO_PANEL=waveshare build    # the badge
idf.py -DHALO_PANEL=crowpanel build    # the development board (the default)
```

It picks the display backend, the pin map, the flash size, the console and the
partition table, and each board keeps its own generated `sdkconfig` so the two
cannot poison each other. Numbers below are tagged with the board they came
from; only the badge's close a gate for the product.

| Badge (Waveshare 3.97) | Measured |
| --- | --- |
| Ordinary screen (whole-panel partial) | 655 ms |
| Every ~10th screen (full refresh + deep clean) | 7746 ms |
| Full refresh on its own | 2573 ms |
| Fast refresh, unused so far | 2167 ms |
| Sub-region partial | never used: this panel places them wrong |
| Text at 235 DPI | Spleen 16 x 32 readable, budget 48 x 12 stands |
| State screens drawn and checked by eye | 19 of 21 |

| Development board (CrowPanel 5.79) | Measured |
| --- | --- |
| Partial refresh (every ordinary screen) | 495 ms |
| Full refresh (clear then draw, ghost flush) | 4309 ms |
| BLE audio throughput | 158 kbit/s, 2.25x realtime |
| Clip delivered, clean link | 233 chunks, 2.21 s, 2.10x realtime, 0 bad CRC |
| Final marker resend | verified: 2 dropped on purpose, resent, segment completed |
| Wallpaper, 48000 bytes over BLE | 341 writes, 4.1 s, stored, CRC matched |
| Host tests | 194, about 20 ms |

## Layout

```
core/      state machine, gesture detector, layout, audio codec and the BLE
           message layer. No board headers, no vendor library, no framework.
           Compiles on a laptop and on the ESP32 unchanged.
board/     the ESP-IDF app: pins, tasks, the radio and the panel. Glue only.
test/      host tests, one tiny harness, no third party dependency.
```

Adding a file to `core/src/` means editing **two** CMakeLists: `firmware/CMakeLists.txt`
for the host build and `firmware/core/CMakeLists.txt` for the ESP-IDF component. The
host tests pass happily while the device build fails to link.

The split exists so the part that must never be wrong (nothing irreversible without
a deliberate confirm) can be tested exhaustively without hardware in the loop.

## Build and test on a computer

```sh
cd firmware
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
./build/halo_tests                 # every test
./build/halo_tests review          # only tests whose name contains "review"
```

Needs only CMake 3.16 and a C++17 compiler. 194 tests, about 20 ms.

## Build and flash the device

```sh
source ~/esp/esp-idf/export.sh
cd firmware/board
idf.py -DHALO_PANEL=waveshare build                       # the badge
idf.py -DHALO_PANEL=waveshare -B build.waveshare build    # or keep the two apart
idf.py -DHALO_BENCH_INJECT=1 build                        # bench build, never ship it
idf.py -DHALO_PANEL=waveshare -p /dev/cu.usbmodem101 flash monitor
```

The port is not stable across replugs, and the two boards do not even look alike:
the badge has no USB to serial bridge, so it is **`/dev/cu.usbmodem*`** (the chip's
own USB-Serial/JTAG), while the development board's CH340 is `/dev/cu.usbserial-*`.
Check `ls /dev/cu.*`. If the badge's port vanishes, give it a minute before blaming
the cable: with no battery attached its PMU can drop the system rail, and the USB
peripheral is inside the chip.

`?` lists the console keys. `C` deep cleans the panel, `T` draws the full panel
probe (rows labelled from A, each starting and ending with its own letter, and a
black block inside each of the four corners; 8 rows on the development board, 12
on the badge), so a region that does not update can be named rather than
described. `F` runs the refresh benchmark: full, fast, partial over a whole
screen and partial over one line, timed and logged.
In the bench build, `?` also lists the keys that inject gestures. `-DHALO_BENCH_INJECT=1` is a CMake
cache variable and therefore **sticky**: once passed, later plain builds stay bench
builds until `idf.py fullclean`. The boot log names the build it is running.

## Check the link against a stand in phone

`tools/phone_sim.py` connects from a Mac as the central, triggers the clip over serial
and acknowledges it the way the contract says. It needs `bleak` and `pyserial`.

```sh
tools/phone_sim.py        # clean run
tools/phone_sim.py 9      # drop every 9th chunk, exercising resend_from
tools/phone_sim.py 0 2    # drop the first two final markers, exercising the 300 ms resend
tools/screen_sim.py       # fragmented Screen and Settings writes, and a broken sequence
tools/screen_tour.py      # walk the nine screens the device owns, to look at them
tools/backend_tour.py     # walk the twelve the backend owns, as real Screen writes
tools/backend_tour.py REVIEW CANCELLED   # or just the ones named
tools/flow_sim.py         # a whole request to an approve, then power cycle and reconcile
tools/wallpaper_sim.py    # send a wallpaper the way the phone does, check the answer
tools/wallpaper_sim.py --drop 7       # drop every 7th chunk: it must ask for a resend
tools/wallpaper_sim.py --break-crc    # a corrupt byte: it must refuse and start over
```

`flow_sim.py` needs the bench build, because it injects gestures over serial. Clear the
flag afterwards with `idf.py fullclean`.

It is not a replacement for Carlos's phone: CoreBluetooth is not Android's stack and the
throughput numbers will differ.

## What the tests pin

Every transition in the V1 table and in the team additions table, plus the two
safety properties the hard constraints call for:

- `no_state_and_no_event_approves_while_the_confirm_guard_is_closed` sweeps all 21
  states against all 27 event kinds and fails if anything produces `SendApprove`.
  `approve_is_reachable_so_the_sweep_is_not_vacuous` keeps that sweep honest.
- `critical_battery_is_entered_from_every_state` does the same for the battery
  override, and checks capture is stopped and execution inhibited.

## BLE messages

Every message in the contract is built and read in `core/src/messages.cpp`, not in the
board glue, so the bytes the phone will receive are pinned by host tests rather than
checked by eye on a radio. `firmware/docs/ble_fields.md` answers Carlos's questions on
the draft and records the device side decisions; the agreed contract itself is BRIEFING
section 2.

Two rules the tests exist to defend:

- A Screen write that cannot be fully understood is dropped whole and the previous
  screen stays on the glass. Drawing the half that parsed would show a screen nobody
  designed, and the user confirms what is on the glass.
- The backend may cancel a request it owns, and the device follows. But a CANCELLED
  screen is ignored once approve has gone out: EXECUTING onwards is reconciled by
  execution key, never cancelled.
- A dispatched action outlives a reboot. The execution key is written to flash as the
  device leaves REVIEW, so BOOT / RECOVERY comes up in RECONCILE STATUS and asks what
  happened instead of returning to READY as if nothing had.
- A fragmented write is reassembled or dropped whole, never partly applied. Each JSON
  write carries a 2 byte prefix and a REVIEW screen takes three of them.
- Once `approve` has gone out the device never reports that request as cancelled, in any
  state, for any reason. EXECUTING onwards is reconciled by execution key. The effect may
  already exist, and "cancelled" is a claim the device cannot make.

## Reading the log

An event that does nothing is logged, with the state it did nothing in:

```
12896  hold            READY -> PHONE / BLUETOOTH CHECK  [draw screen]  hold: new request
12896  phone ready     PHONE / BLUETOOTH CHECK -> RECORDING  [start capture, draw screen]
19956  screen: review  ignored in READY
```

That last line used to be invisible. It was an `ESP_LOGD`, and the build's maximum
log level is INFO, so the macro was compiled out and `esp_log_level_set` could not
bring it back. Tick and ScreenPresented are still left out: the device sends both
to itself constantly and by design.

## The screens the device owns

Twelve of the 21 states get their words from the backend, which owns the request
and therefore the wording. The other nine are the device's own: BOOT / RECOVERY,
READY, WALLPAPER, PHONE / BLUETOOTH CHECK, PHONE DISCONNECTED, RECORDING, RECORD
ADDITION, FINALIZE AUDIO and CRITICAL BATTERY. Nothing sends text for those, so
`core/src/screens.cpp` is where their words live.

In `core/` rather than the board glue for the usual reason: the backend checks its
`screen_lines` against the text budget before sending them, and nothing would
check ours. The tests hold every local line to **28 characters**, the narrower
portrait budget, so one set of words fits both orientations, and lay every screen
out against all three panel shapes to prove nothing falls off the bottom.

`local_screen()` returns false for the twelve the phone owns. Inventing words for
those would put text on the glass that the backend never wrote and never checked.

## The wallpaper

The picture the user picks on the phone, over the Wallpaper characteristic: JSON
chunks with an offset and a CRC, base64 inside (BRIEFING section 2, field names in
`docs/ble_fields.md`). 48000 bytes, 141 per write at MTU 256, 341 writes.

Split the way everything else here is split. `core/src/wallpaper.cpp` decides what to
accept, which chunk to ask for again and when an image may be stored, and is tested on
a laptop against a fake sink. `board/main/wallpaper_store.cpp` owns the flash and
nothing else: the image is assembled in RAM and written in one erase and one write
after the last chunk, into whichever of two slots is not being shown, with the record
that marks a slot complete written **last**. The picture on the glass therefore
survives a transfer that dies halfway, a bad CRC, or the power going out mid-write;
the old slot stays in charge until the new one is whole.

Three things that are easy to get backwards:

- The image is stored exactly as the phone sent it, where **1 means white**. The
  display list is the opposite, so `BitmapCmd::invert` flips it at draw time. Keeping
  the wire, flash and preview bytes identical is what makes them comparable when a
  picture comes out wrong.
- The device answers **once** per transfer. One lost chunk puts every later chunk at
  the wrong offset, and answering each of those would be 300 notifications for one
  lost packet.
- The development panel shows 272 of the canvas's 480 rows, so it **squeezes** the
  image rather than cropping it: the user chose a picture, not the top half of one.
  The badge shows all 480 and squeezes nothing.

## The confirm guard

The 1 s guard starts on `EventKind::ScreenPresented`, which the display backend
sends when its `present()` call returns, not when the screen was queued. On e-paper
those two moments are hundreds of milliseconds apart, so a guard started at queue
time would be partly spent before the user could see anything.
`review_guard_runs_from_the_present_call_not_from_entering_review` pins it.

If `ScreenPresented` never arrives, the guard never opens and the single press is
ignored: the failure direction is "cannot approve", never "approves by accident".

## Timing constants

All in `core/include/halo/config.hpp`, every one marked PROVISIONAL until measured
on the final board. Nothing in the code reads a time literal.
