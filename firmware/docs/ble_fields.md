# BLE field names: the device side (Victor)

**Status:** answers to the five open questions in `phone/docs/ble_fields_draft.md`, plus the
decisions made while implementing the contract. 2026-10-16.

The field names themselves were **agreed on 2026-10-13** and adopted in BRIEFING section 2.
This file does not reopen any of that. It answers the questions still sitting at the bottom of
Carlos's draft, records what the firmware actually does, and lists the four things the contract
turns out not to cover.

Implementation: `firmware/core/include/halo/messages.hpp` and `core/src/messages.cpp`, with the
radio in `board/main/ble.cpp` and the glue in `board/main/main.cpp`. Every message is built and
read in `core/`, not in the glue, so all of it is tested on a computer against the exact bytes
the phone expects. Verify: `cd firmware && cmake --build build && ./build/halo_tests messages`.

## Answers

**1. The UUIDs and properties: yes, unchanged.** They are what the firmware has been
advertising since 2026-10-13, byte for byte (`board/main/ble.cpp`, the `HALO_UUID` macro).
NimBLE wants the 16 bytes least significant first, which is the reverse of how the UUID is
written down, so the macro looks nothing like the string in your table; a test is not possible
without a radio, but the phone connecting at all proves it.

**2. `boot`, `hello` on subscribe, and `phone_ready`: yes to all three.**

- `boot` is `esp_random()`, new on every power up, and it is a **full u32**. Read it as
  unsigned: a value above 2^31 is not a negative number. The firmware writes it with an
  unsigned path for exactly this reason, and a test pins `0xDEADBEEF` coming out as
  `3735928559`.
- `hello` goes out from the subscribe callback, not from the connect callback. Your reasoning
  was right and the firmware already depended on it: a notification sent before anyone has
  subscribed is dropped by the stack and nobody ever sees it.
- `phone_ready` is adopted as drafted. The device leaves PHONE / BLUETOOTH CHECK for RECORDING
  only when the link is up **and** the last `phone_ready` was true; anything else sends it to
  PHONE DISCONNECTED. A BLE disconnect sets the stored value back to false, so a reconnect
  cannot inherit a stale "ready" from before the drop.

**3. The 300 ms final marker resend: yes.** It is bounded by the device's delivery deadline
(`delivery_deadline_ms`, 10 s, PROVISIONAL in `core/include/halo/config.hpp`), and when that
expires the device goes to FAILURE rather than resending forever.

**4. Keep the 2 byte prefix, outside the JSON.** Three reasons. The receiver can tell whether
it has a whole message without parsing anything, which matters because a fragment is not valid
JSON on its own. The audio path stays untouched, and it is the one that has to be cheap. And a
sequence number inside the JSON would have to be stripped before parsing, which means every
message would be rewritten in memory on arrival. Audio Ack stays a single unprefixed write, as
drafted. Already implemented in `ble::notify_event`.

**5. Yes: four things are missing or ambiguous.** The first one is a real hole.

### 5a. Nothing in the contract ever gives the device a `pending_key` (needs your sign off)

The device is required to report `pending_key` in `hello`, `dismiss` and `status_query`. The
backend creates the execution key. No message in the contract carries it from the phone to the
device, so as written the device can only ever send `null`, and BOOT / RECOVERY has nothing to
reconcile from.

**Proposal: add an optional `pending_key` to the Screen write.**

**Correction, 2026-10-16: it has to be on the REVIEW screen, not EXECUTING.** I said EXECUTING
when I first wrote this, and that is one screen too late. `PersistExecutionKey` fires in the
same transition that sends `approve`, as the device *leaves* REVIEW, so the key has to already
be in hand when the user confirms. A key that only arrives with the EXECUTING screen is written
after the action was dispatched, and a reboot in that window is exactly the case the key exists
for. The device clears it on `ClearExecutionKey`; both are already actions in the V1 table.

Carrying it on EXECUTION_UNCERTAIN as well, after a reconnect, is still worth having.

The firmware reads it already, as an optional field. Until the phone sends one, nothing
changes: the field is simply never present. If you would rather carry it another way, say so
and I will move it, but something has to carry it.

### 5b. `hello`'s `state` uses the same spelling rule as Screen's `screen`

Your draft says "V1 state name" for `hello` and "upper case with underscores" for `screen`.
The firmware uses the second spelling for both, by this rule:

> Take the spec name from BRIEFING section 2, upper case it, and replace every run of
> characters that are not letters or digits with one underscore.

So `CONFLICT-AMBIGUOUS` becomes `CONFLICT_AMBIGUOUS`, `READ-ONLY RESULT` becomes
`READ_ONLY_RESULT`, `BOOT / RECOVERY` becomes `BOOT_RECOVERY`, and
`PHONE / BLUETOOTH CHECK` becomes `PHONE_BLUETOOTH_CHECK`. Every name in your Screen list comes
out identical under that rule, so nothing you have written changes. The three that were never
in your list are the device's own states, and `hello` can report any of the 21.

A test derives the expected spelling from the spec name for all 21 states and fails if the
firmware's table drifts from the rule, so you can implement the rule rather than copy a table.

### 5c. `transport` and `battery` cancels, and the one case that must never be a cancel

The V1 state machine emits a cancel action on only two paths: a double press, and the 60 s
expiry. The other two reasons we agreed on are real situations the machine does not have an
action for, so the device sends them from the transition itself: entering CRITICAL BATTERY
(`battery`), and a link loss or buffer exhaustion (`transport`).

**One exception, and it matters.** Once `approve` has gone out, the device never reports a
cancel for that request, whatever happens to it: EXECUTING, EXECUTION UNCERTAIN and RECONCILE
STATUS are reconciled by execution key, never cancelled. The effect may already exist, and
"cancelled" is a claim the device cannot make. A double press in EXECUTION UNCERTAIN therefore
sends `dismiss`, never `cancel`, and the key stays saved on both sides.

### 5d. `FAILURE` means two different things and the screen name cannot tell them apart

A FAILURE screen after an approve means the execution was rejected (authoritative, no effect).
A FAILURE screen before one means processing failed (speech to text, the model, or a deadline).
They differ in whether the pending execution key may be cleared, so the device cannot treat
them alike.

The firmware resolves it from its own state: FAILURE while EXECUTING, EXECUTION UNCERTAIN or
RECONCILE STATUS is a rejection, and FAILURE anywhere else is a processing failure. That works
as long as the phone never sends FAILURE for a processing error **after** an approve has gone
out. If the backend can do that, we need a field on the screen to say which it is. Can it?

Related and smaller: a CANCELLED screen from the phone (the backend cancelled, not the user)
has no event in the V1 table. The device draws it and goes no further. Fine for now; worth a
row in the table if the backend can really originate one.

## What the device sends, as implemented

Every message below is pinned byte for byte by a host test.

| type | JSON |
| --- | --- |
| `hello` | `{"type":"hello","fw":"f2-dev","boot":3735928559,"state":"READY","req":12,"sid":null,"pending_key":null,"battery":87}` |
| `wake` | `{"type":"wake"}` |
| `record_start` | `{"type":"record_start","req":12,"seg":3,"rev":2,"addition":false}` |
| `record_end` | `{"type":"record_end","req":12,"seg":3,"chunks":233,"ms":4660}` |
| `approve` | `{"type":"approve","sid":42}` |
| `select` | `{"type":"select","sid":42,"index":2}` |
| `continue` | `{"type":"continue","sid":42}` |
| `cancel` | `{"type":"cancel","req":12,"sid":42,"reason":"user"}` |
| `dismiss` | `{"type":"dismiss","sid":42,"pending_key":"01HQ..."}` |
| `ack` | `{"type":"ack","sid":42}` |
| `status_query` | `{"type":"status_query","pending_key":"01HQ..."}` |
| `activity` | `{"type":"activity","sid":42}` |
| `battery` | `{"type":"battery","battery":9}` |

Field order is stable, but nothing should depend on it: it is JSON.

`sid` and `pending_key` are `null`, never absent, when the device has none. A cancel in
PHONE / BLUETOOTH CHECK happens before anything was drawn and carries `"sid":null`, which is
not the same as screen 0.

A message that does not fit its buffer is **not sent at all** and is logged as an error. A
truncated object is not JSON, and half an approve is worse than no approve.

## What the device does with what you send

- **Screen.** A write with a `screen` the device does not know, or with no `sid`, is dropped
  whole and the previous screen stays on the glass: drawing the half that parsed would show the
  user a screen nobody designed. A write carrying only `sid` and `status_line` is the partial
  refresh path and leaves the body alone (495 ms on the development panel, against 4.3 s for a
  clear and redraw). `screen_lines` longer than the text budget are cut and the fact is logged
  as an error, because it means the backend and the device disagree about the budget.
- **`screen_lines` content is arbitrary user text** and is read as data, never as instructions.
  Commas, quotes, backslashes and brackets inside a line survive intact; a reader that split the
  array on commas would cut a line in half and the user would approve something they never read.
  There is a test for exactly that.
- **Settings.** Absent fields keep their previous value, as drafted. An `orientation` that is
  not `portrait` is treated as landscape, so a typo cannot leave the panel in a third state.
- **Audio Ack.** `upto` is required; `req`, `seg`, `resend_from` and `final` are optional. An
  ack that cannot be read is ignored and logged, never guessed at.

## Carlos's notes on the test streamer, 2026-10-16

Different layer from the questions above: these are about `AudioTx` and the `A` key, not the
field names. All three are now dealt with, and the two that mattered are **verified on the
board**, not reasoned about.

**1. The final marker was only ever sent once. Fixed, and it was a real hang.** You were right:
`final_sent_` latched and nothing ever resent it. It now goes out every 300 ms
(`final_marker_resend_ms` in `core/include/halo/config.hpp`) until the phone acknowledges it,
bounded by `delivery_deadline_ms` (10 s), after which the device reports a transport failure to
the state machine rather than resending forever.

Verified on the CrowPanel with `tools/phone_sim.py`, which now takes a second argument: the
number of final markers to throw away on purpose.

```
tools/phone_sim.py 0 2     # drop the first two final markers
dropping final marker #1 on purpose; the device should resend it
dropping final marker #2 on purpose; the device should resend it
final markers seen 3 (2 dropped on purpose)
chunks stored 233, bad CRC 0, wall time 2.75 s, 1.69x realtime
```

The clean run is 2.21 s and the two dropped markers cost about 540 ms, which is the 300 ms
timer twice. Before the fix this run would have hung exactly as you described.

**2. `record_start` and `record_end`. Both fixed.** Measured on the board, same session:

```
event: {'type': 'record_start', 'req': 513, 'seg': 3, 'rev': 2, 'addition': False}
event: {'type': 'record_end', 'req': 513, 'seg': 3, 'chunks': 233, 'ms': 4650}
```

`record_end` is sent when the **last chunk is queued**, which is the stand-in for the button
release, so your release-to-complete number is the gap between that event and the final ack.
Note that is deliberately not "when the button came up": on a stalled link those are not the
same moment, and the thing you are timing is delivery.

One thing to expect so it does not look like a bug: **`hello` says `req: 0` while the audio says
`req: 513`.** The device's request counter only moves when a hold leaves READY, and the `A` key
is a bench shortcut that does not go through the state machine. 513, rev 2, seg 3 are the IDs
the golden vectors in `phone/test/golden/adpcm_chunks.json` were built with, so the clip path
uses them to stay byte comparable. A real recording will carry a real counter.

**3. The `battery` event: yes, exactly what you implemented.**
`{"type":"battery","battery":9}`, percent in a `battery` field, matching `hello`. That is what
the firmware builds and there is a test pinning the bytes.

"Threshold crossings" means the device sends one when it crosses between these buckets, not on
every reading: **above 50, 26 to 50, 11 to 25, and 10 or below.** The last boundary is the one
that matters, because 10% or less is CRITICAL BATTERY (raised from the spec's 5% so the badge's
PMU cannot power off mid-reconciliation). The others are there so you can show a falling battery
without polling. If you would rather have a different set, they are one edit.

**On flashing for the test day:** you do not need the bench build. The `A` key is outside the
`HALO_BENCH_INJECT` guard, and `phone_sim.py` triggers it over serial, so an ordinary
`idf.py -p /dev/cu.usbserial-110 flash monitor` is enough. Worth knowing anyway: that flag is
sticky in the CMake cache, so once it has been passed, later plain builds stay bench builds
until `idf.py fullclean`. The boot log names the build; trust that over the command line.

## The 2 byte prefix was never removed on the way in, 2026-10-16

Carlos found this by reading the new message layer, and he is right. `notify_event` added the
prefix on the way out, but `on_screen` and `on_settings` handed the raw write straight to the
parser, prefix and all, and nothing ever put fragments back together.

It had not shown up because the JSON reader looks for keys anywhere in the buffer, so two junk
bytes at the front were simply skipped. Everything tested so far fits in one write at MTU 256,
so everything worked. A full REVIEW screen is 682 bytes and goes out as three writes, and each
one would have been treated as a separate screen.

**Fixed, with the rules as proposed.** `msg::Reassembler` in `core/src/messages.cpp`, one
instance per characteristic so Screen and Settings cannot confuse each other's message IDs, and
both are reset on disconnect so a half assembled write from the old link cannot be completed by
the new one. Fragment 0 starts a message; every fragment after it must be the previous number
plus one and carry the same message ID; bit 7 ends it; anything else is dropped whole and
logged. The ceiling is 1 KB. Only whole messages are handed to the main task.

Eight host tests cover it, including the three fragment REVIEW screen you asked for. Verified on
the board as well, with a new `tools/screen_sim.py` that writes the way the phone does:

```
settings, 111 bytes, 1 fragment
  device: settings: orientation=landscape tz=America/Chicago time=2026-10-16T18:00:00-05:00
  device: settings: phone_ready=1

REVIEW screen, 682 bytes, 3 fragments        -> accepted

too many lines, 349 bytes, 4 fragments
  device: screen 43 carried more lines than the text budget allows

broken sequence: fragment 1 skipped
  device: screen write: fragment number skipped
  device: screen write dropped, nothing applied
```

The third case is the one that proves it: that error can only be raised once the last fragment
has been joined on and the JSON parsed.

Two things worth knowing from that session:

- **A screen that is accepted logs nothing** at the default log level, because the state machine
  ignoring a REVIEW screen in READY is a debug line. Silence there means "not dropped".
- `screen_sim.py` is separate from `phone_sim.py` on purpose. Reading the device's serial log
  right after an audio run turned out not to work reliably, and a test harness that prints
  nothing when it fails is worse than no harness. `screen_sim.py` says so out loud instead.

You were right that it does not block P1 and needed to be in before P2. It is in now.

## Still open, and not mine to close

- `pending_key` on the Screen write: **agreed with Carlos 2026-10-16**, optional on the Screen
  write, and it must be on **REVIEW** (see the correction above). The device side is finished and
  verified: the key is written to flash as the device leaves REVIEW, survives a power cycle, and
  brings the device up in RECONCILE STATUS instead of READY. It needs the backend to send one.
- The FAILURE ambiguity (5d) is with Orlando: the backend decides whether a FAILURE screen can
  follow an approve. Until it is answered the device infers it from its own state.
- The ring buffer size stays provisional until F2 runs on the badge with a real microphone.
- MTU and throughput on Android are still unmeasured. Everything here was exercised against a
  Mac acting as the central; CoreBluetooth is not Android's stack.
