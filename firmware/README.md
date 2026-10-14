# HaloAI firmware (Victor, branch `Hardware`)

The device: hold the side button, speak, read the e-paper screen, confirm with a
single press. See `docs/BRIEFING.md` section 2 for the V1 state machine and the BLE
contract, and section 3.3 for the phases.

**State: F0 in progress.** The hardware independent layer is built and tested on a
computer. There is no board code yet: the framework (Arduino core or ESP-IDF) is not
chosen, and writing board code before that choice would mean writing it twice.

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
ctest --test-dir build --output-on-failure
```

Needs only CMake 3.16 and a C++17 compiler. 89 tests, about 20 ms.

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
