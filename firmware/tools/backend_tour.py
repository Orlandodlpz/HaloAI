#!/usr/bin/env python3
"""Walk the twelve screens the BACKEND owns, so they can be looked at.

screen_tour.py walks the nine screens the device writes itself. These twelve get
their words from the backend over BLE, so nothing in the firmware decides how
they read: the only way to see them is to send real Screen writes and look at
the glass. screen_sim.py already proves fragmented writes reassemble; this is
about whether the screens are legible and well laid out, which no test can say.

The text below is deliberately realistic and deliberately near the limit: 48
characters is the landscape budget, so lines of 46 to 48 characters are what a
real long draft looks like when the backend has wrapped it.

Needs the bench build, because it injects the gestures that walk the device into
the states these screens belong to (a REVIEW screen is refused anywhere but
PROCESSING, by design):

    idf.py -DHALO_PANEL=waveshare -DHALO_BENCH_INJECT=1 build flash
    tools/backend_tour.py [seconds per screen]

RECONCILE STATUS is not here: it is only entered at boot with a pending
execution key in flash, which is what flow_sim.py exercises.
"""
import asyncio
import json
import sys
import time

import serial
from bleak import BleakClient, BleakScanner

from serial_port import find_port

HOLD = 5.0
ONLY: list[str] = []
for arg in sys.argv[1:]:
    try:
        HOLD = float(arg)
    except ValueError:
        ONLY.append(arg.upper())   # e.g. tools/backend_tour.py CANCELLED REVIEW

BASE = "78450000-6bb6-46e1-be3c-499c2950b6ac"
SCREEN = BASE.replace("0000", "0004", 1)
SETTINGS = BASE.replace("0000", "0006", 1)

# Walking into PROCESSING, which is where most of these screens are accepted
# from. Every entry does this for itself rather than inheriting the state the
# entry before it left behind: the badge has no microphone, so a segment that
# was started is empty, hits its delivery deadline about ten seconds later and
# drops the device into FAILURE and then READY underneath the tour. Screens then
# get refused with "ignored in READY" for a reason that has nothing to do with
# the screen being walked.
# The leading double press settles whatever the run before left behind: it
# cancels a decision state and acknowledges an outcome one, both of which end at
# READY, where a hold is accepted. Without it the arm is silently ignored and
# the screen is then refused "in READY", which looks like a problem with the
# screen rather than with the three states in front of it.
ARM = [b"2", b"h", b"p", b"R", b"a"]
# Stops one step earlier, in FINALIZE AUDIO. The PROCESSING screen is what MOVES
# the device out of FINALIZE AUDIO, so arming all the way to PROCESSING first
# gets it refused: "screen PROCESSING ignored in PROCESSING". The backend sends
# it while the device is still finalising, which is what this reproduces.
ARM_FINALIZING = [b"2", b"h", b"p", b"R"]

# (keys to inject first, screen JSON, what should be readable on the panel)
TOUR = [
    # Progress during PROCESSING is a STATUS-ONLY write. A PROCESSING screen
    # carrying a body is refused ("screen PROCESSING ignored in PROCESSING"),
    # and rightly so: there is no transition behind it. The status bar path is
    # the one the backend actually uses, and it is a small region, so it stays a
    # true partial refresh rather than being promoted to a full one.
    (ARM_FINALIZING,
     {"screen": "PROCESSING", "status_line": "Checking your calendar",
      "screen_lines": ["Working on it..."]},
     "PROCESSING, arriving while the device is still in FINALIZE AUDIO"),

    # A real status bar update carries NO "screen" field: status_only is
    # (no state AND no lines), so adding the state turns it back into a full
    # screen write with a transition behind it. This is the only path that stays
    # a true partial refresh, because it is the only change small enough.
    ([], {"_status_only": True, "status_line": "Searching the web"},
     "status bar only: 'Searching the web', the body must NOT be redrawn"),

    (ARM, {"screen": "PROCESSING_SLOW", "status_line": "Still working",
          "screen_lines": ["This is taking longer than usual.", "",
                           "Double press to cancel."]},
     "PROCESSING SLOW"),

    (ARM, {"screen": "REVIEW", "status_line": "Ready to confirm", "rev": 1,
          "execution_key": "01HQZX3K7M4N8P2Q",
          "screen_lines": [
              "Create event",
              "",
              "Gym",
              "Tomorrow, Tue 28 Oct, 3:00 - 4:00 PM",
              "",
              "Press to confirm, double press to cancel."]},
     "REVIEW, the main one: press to confirm must be unmistakable"),

    (ARM, {"screen": "REVIEW", "status_line": "Ready to confirm", "rev": 2,
          "execution_key": "01HQZX3K7M4N8P2Q",
          "screen_lines": [
              "Send email to victor.munozvera01@utrgv.edu",
              "",
              "Subject: Lab moved to Thursday",
              "",
              "Hi Victor, the meeting moved to Thursday",
              "at 3pm because the lab is booked. Let me",
              "know if that does not work for you and I",
              "will find another slot. Thanks, Orlando",
              "",
              "Press to send, double press to cancel."]},
     "REVIEW with a full email: the long address must not be cut"),

    (ARM, {"screen": "CONFLICT_AMBIGUOUS", "status_line": "Which one?",
          "screen_lines": ["Three people match \"Victor\"."],
          "candidates": ["Victor Munoz  victor.munozvera01@utrgv.edu",
                         "Victor Reyes  vreyes@example.com",
                         "Victor Salas  v.salas@example.com"]},
     "CONFLICT-AMBIGUOUS, highlight bar on the first candidate"),

    (ARM, {"screen": "CONFLICT_SCHEDULE", "status_line": "That time is busy",
          "screen_lines": [
              "You already have Calculus II then.",
              "",
              "Tue 28 Oct, 3:00 - 4:15 PM",
              "",
              "Press to book it anyway, double press to",
              "cancel."]},
     "CONFLICT-SCHEDULE"),

    (ARM, {"screen": "READ_ONLY_RESULT", "status_line": "Your Tuesday",
          "screen_lines": [
              "Tuesday 28 October",
              "",
              "9:00  Calculus II, Engineering 1.204",
              "11:00 Office hours with Dr. Alvarez",
              "15:00 Senior design standup",
              "18:30 Gym",
              "",
              "Nothing after 8 PM."]},
     "READ-ONLY RESULT: nothing here may look confirmable"),

    (ARM, {"screen": "CANCELLED", "status_line": "Cancelled",
          "screen_lines": ["Nothing was sent.", "", "Hold to try again."]},
     "CANCELLED"),
]

# From REVIEW onwards: approve with a single press, then the outcomes.
AFTER_APPROVE = [
    (ARM,
     {"screen": "REVIEW", "status_line": "Ready to confirm", "rev": 1,
      "execution_key": "01HQZX3K7M4N8P2Q",
      "screen_lines": ["Create event", "", "Gym",
                       "Tomorrow, Tue 28 Oct, 3:00 - 4:00 PM", "",
                       "Press to confirm, double press to cancel."]},
     "REVIEW again, to approve from"),

    ([b"1"], {"screen": "EXECUTING", "status_line": "Sending",
              "screen_lines": ["Creating the event..."]},
     "EXECUTING (the single press takes about 400 ms to land: the double "
     "press window has to close first)"),

    ([], {"screen": "SUCCESS", "status_line": "Done",
          "screen_lines": ["Event created.", "", "Gym, tomorrow 3:00 - 4:00 PM"]},
     "SUCCESS"),
]

UNCERTAIN = [
    (ARM,
     {"screen": "REVIEW", "status_line": "Ready to confirm", "rev": 1,
      "execution_key": "01HQZX3K7M4N8P2Q",
      "screen_lines": ["Send email to Victor", "", "Subject: Lab moved", "",
                       "Press to send, double press to cancel."]},
     "REVIEW, to approve from again"),

    ([b"1"], {"screen": "EXECUTION_UNCERTAIN", "status_line": "Not sure",
              "screen_lines": [
                  "The email may or may not have been sent.",
                  "",
                  "Check your phone before sending it again."]},
     "EXECUTION UNCERTAIN: this must never read like a success"),
]

FAILURES = [
    (ARM,
     {"screen": "FAILURE", "status_line": "Something went wrong",
      "screen_lines": ["That took too long.", "", "Please try again."]},
     "FAILURE from PROCESSING (nothing was dispatched)"),
]


def fragments(doc: bytes, msg_id: int, size: int = 180):
    """The 2 byte prefix every JSON write carries: id, then the last-fragment
    flag in bit 7 and the fragment number in bits 0 to 6."""
    pieces = max(1, -(-len(doc) // size))
    for i in range(pieces):
        last = 0x80 if i == pieces - 1 else 0
        yield bytes([msg_id, last | i]) + doc[i * size:(i + 1) * size]


def drain(ser, seconds):
    """Poll in_waiting in a loop.

    One read() after a sleep silently returns almost nothing on this link and
    looks exactly like a board that logged nothing. That is how a run of this
    tour reported every screen as fine while the device was in fact refusing
    them and the panel kept showing the screen before: the refusals were in the
    log and the log was never read.
    """
    buf, end = b"", time.time() + seconds
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
        else:
            time.sleep(0.05)
    return buf.decode("utf-8", "replace")


async def show(client, ser, msg_id, keys, screen, expect, index, total):
    for k in keys:
        ser.write(k)
        ser.flush()
        # Long enough for a full refresh to finish: a body change costs about
        # 2.5 s on this panel, and a key sent into the middle of one just queues.
        time.sleep(2.0)
        drain(ser, 0.2)
    body = {k: v for k, v in screen.items() if k != "_status_only"}
    doc = json.dumps({"sid": 500 + index, **body}).encode()
    ser.reset_input_buffer()
    for f in fragments(doc, msg_id):
        await client.write_gatt_char(SCREEN, f, response=True)
    log = drain(ser, 3.0)

    # Only a refusal OF THIS SCREEN counts. The device logs "<event> ignored in
    # <state>" for all sorts of unrelated events that arrive while a screen is
    # up (a stray transport failure, a late gesture), and treating those as a
    # refusal reported screens as missing that were on the glass the whole time.
    refused = [l.strip() for l in log.splitlines()
               if ("screen " in l and "ignored in" in l and "W (" in l)
               or "rejected" in l or "budget" in l]
    drew = [l.strip() for l in log.splitlines() if "panel: present" in l]
    if refused:
        verdict = "REFUSED, the panel still shows the screen before this one"
    elif drew:
        verdict = "drew: " + drew[-1].split("panel: ")[-1]
    else:
        verdict = "NO REFRESH LOGGED, treat as not shown"
    label = screen.get("screen", "STATUS_BAR_ONLY")
    print(f"\n[{index}/{total}] {label:20s} {verdict}")
    print(f"     expect: {expect}")
    for line in refused[:2]:
        print("     ", line)
    time.sleep(HOLD)
    return bool(drew) and not refused


async def main():
    ser = serial.Serial(find_port(), 115200, timeout=0.2)
    print("scanning for HaloAI ...")
    dev = await BleakScanner.find_device_by_name("HaloAI", timeout=15.0)
    if dev is None:
        print("NOT FOUND: not advertising, or Bluetooth permission is denied")
        return 1

    async with BleakClient(dev) as client:
        print(f"connected, mtu {client.mtu_size}")
        settings = json.dumps({"phone_ready": True, "orientation": "landscape",
                               "time": "2026-10-27T21:30:00-05:00",
                               "tz": "America/Chicago"}).encode()
        await client.write_gatt_char(SETTINGS, next(fragments(settings, 1)), response=True)
        time.sleep(0.5)

        groups = [("main run", TOUR), ("approve and succeed", AFTER_APPROVE),
                  ("approve and go uncertain", UNCERTAIN), ("failure", FAILURES)]
        if ONLY:
            kept = []
            for title, group in groups:
                if not any(e[1].get("screen", "STATUS_BAR_ONLY") in ONLY for e in group):
                    continue
                # The main run's entries each re-arm themselves, so they can be
                # picked out one at a time. The approve groups are sequences:
                # EXECUTION_UNCERTAIN only exists after the REVIEW and the press
                # in front of it, so keep those groups whole or the screen is
                # reported missing for a reason that is purely this filter's.
                kept.append((title, [e for e in group
                                     if e[1].get("screen", "STATUS_BAR_ONLY") in ONLY]
                             if title == "main run" else group))
            groups = kept
        total = sum(len(g) for _, g in groups)
        if total == 0:
            print(f"nothing matches {ONLY}")
            return 1
        print(f"\nWatch the panel: {total} screens, about {HOLD:.0f} s each.\n"
              f"Every one should be readable at arm's length with nothing cut off.")
        i, msg_id = 0, 20
        shown: list[str] = []
        missed: list[str] = []
        for title, group in groups:
            print(f"\n=== {title} ===")
            for keys, screen, expect in group:
                i += 1
                msg_id = (msg_id + 1) % 128
                ok = await show(client, ser, msg_id, keys, screen, expect, i, total)
                (shown if ok else missed).append(screen.get("screen", "STATUS_BAR_ONLY"))
            # Back to READY between groups: a double press cancels anything
            # pre-execution, and an outcome screen takes any input.
            ser.write(b"2")
            ser.flush()
            time.sleep(2.0)
    print(f"\n{len(shown)} of {len(shown) + len(missed)} screens actually reached the glass.")
    if missed:
        print("NOT SHOWN: " + ", ".join(missed))
        print("Re-run just those, e.g.  tools/backend_tour.py " + " ".join(missed[:3]))
    print("Anything that READ badly (rather than never appearing) is a layout fix.")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
