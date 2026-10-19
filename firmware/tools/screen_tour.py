#!/usr/bin/env python3
"""Walk the nine screens the device owns, so they can be looked at.

Twelve of the 21 states get their words from the backend. These nine do not, so
nothing but a pair of eyes checks that they read well on the glass. The host
tests hold them to the text budget and prove they lay out without overflowing;
this is the part that cannot be automated.

Needs the bench build, because it injects gestures over serial:

    idf.py -DHALO_BENCH_INJECT=1 build flash
    tools/screen_tour.py [seconds per screen]
    idf.py fullclean && idf.py build flash     # the flag is sticky, so clear it

WALLPAPER is not in the tour: it needs a minute of idling, and its picture is an
open F1 item anyway.
"""
import sys
import time

import serial

from serial_port import find_port

HOLD = float(sys.argv[1]) if len(sys.argv) > 1 else 4.0

# keys to press, what the state is, what should be readable on the panel
TOUR = [
    ([b"B"], "READY", "Hold the side button / and say what you need, plus a Try: hint"),
    ([b"h"], "PHONE / BLUETOOTH CHECK", "Reaching your phone"),
    ([b"p"], "RECORDING", "Listening... with 'Let go when you are done' at the foot"),
    ([b"R"], "FINALIZE AUDIO", "Sending what you said"),
    ([b"a", b"h"], "RECORD ADDITION", "Listening... / Adding to your request"),
    ([b"R", b"a", b"2"], "CANCELLED", "the cancellation notice, then READY on its own"),
    ([b"h", b"P"], "PHONE DISCONNECTED", "Your phone is not / connected, and how to fix it"),
    ([b"1"], "READY", "back to ready"),
    ([b"b"], "CRITICAL BATTERY", "Battery at 4% / Charge the device"),
    ([b"B"], "READY", "recovered"),
]


def drain(ser, seconds):
    buf, end = "", time.time() + seconds
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf += ser.read(n).decode(errors="replace")
        time.sleep(0.05)
    return buf


def main():
    ser = serial.Serial(find_port(), 115200, timeout=0.2)
    print(f"Watch the panel. Each screen holds for about {HOLD:.0f} seconds.\n"
          f"Every screen should be readable at arm's length with no text cut off.")
    time.sleep(5)  # the boot clear takes about 4 s
    for keys, state, expect in TOUR:
        ser.reset_input_buffer()
        for k in keys:
            ser.write(k)
            time.sleep(0.5)
        out = drain(ser, HOLD)
        print(f"\n{state}\n   expect: {expect}")
        for line in out.splitlines():
            if "halo:" in line and "->" in line:
                print("    ", line.split("halo: ")[-1].strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
