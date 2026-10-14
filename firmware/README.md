# HaloAI firmware (Victor, branch `Hardware`)

The device: hold the side button, speak, read the e-paper screen, confirm with a
single press. See `docs/BRIEFING.md` section 2 for the V1 state machine and the BLE
contract, and section 3.3 for the phases.

**State: F0 done, F1 nearly done, part of F2 built early.** Framework is ESP-IDF
(v5.5.5). The board app runs on the development board: the state machine drives real
buttons, the e-paper renders real screens, and BLE streams audio to a phone.

Everything measured here is on the **Elecrow CrowPanel 5.79**, the development board.
The badge (Waveshare ESP32-S3 e-Paper 3.97) is not in hand, so no number below
closes a phase gate for the product.

| | Measured |
| --- | --- |
| Partial refresh (every ordinary screen) | 495 ms |
| Full refresh (clear then draw, ghost flush) | 4309 ms |
| BLE audio throughput | 158 kbit/s, 2.25x realtime |
| Host tests | 121, about 20 ms |

## Layout

```
core/      state machine and gesture detector. No board headers, no vendor library,
           no framework. Compiles on a laptop and on the ESP32 unchanged.
test/      host tests, one tiny harness, no third party dependency.
```

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

Needs only CMake 3.16 and a C++17 compiler. 121 tests, about 20 ms.

## Build and flash the device

```sh
source ~/esp/esp-idf/export.sh
cd firmware/board
idf.py build                                  # the real build
idf.py -DHALO_BENCH_INJECT=1 build            # bench build, console keys, never ship it
idf.py -p /dev/cu.usbserial-210 flash monitor
```

The serial port number is not stable across replugs: check `ls /dev/cu.usbserial-*`.
In the bench build, `?` lists the keys that inject phone events and gestures, `T`
draws a text probe and `C` deep cleans the panel.

## What the tests pin

Every transition in the V1 table and in the team additions table, plus the two
safety properties the hard constraints call for:

- `no_state_and_no_event_approves_while_the_confirm_guard_is_closed` sweeps all 21
  states against all 27 event kinds and fails if anything produces `SendApprove`.
  `approve_is_reachable_so_the_sweep_is_not_vacuous` keeps that sweep honest.
- `critical_battery_is_entered_from_every_state` does the same for the battery
  override, and checks capture is stopped and execution inhibited.

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
