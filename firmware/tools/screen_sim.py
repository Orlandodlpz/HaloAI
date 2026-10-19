#!/usr/bin/env python3
"""Write Screen and Settings to the device the way the phone really does, split
across fragments, and print what the device logged about it.

Every JSON write in both directions carries a 2 byte prefix: byte 0 is a message
ID, byte 1 has the last-fragment flag in bit 7 and the fragment number in bits 0
to 6 (BRIEFING section 2, "Large JSON"). A full REVIEW screen is about 700 bytes
and goes out as three writes at MTU 256, so this is the path a real screen takes
and the one that used to be silently mis-assembled.

Kept separate from phone_sim.py on purpose: that one streams audio first, and
reading the serial log reliably afterwards turned out not to work.

    tools/screen_sim.py              # a good screen, then a broken sequence
    tools/screen_sim.py 5            # split the screen into 5 fragments
"""
import asyncio, json, sys, time

import serial
from bleak import BleakClient, BleakScanner

from serial_port import find_port

PORT = find_port()
BASE = "78450000-6bb6-46e1-be3c-499c2950b6ac"
SCREEN = BASE.replace("0000", "0004", 1)
SETTINGS = BASE.replace("0000", "0006", 1)

PIECES = int(sys.argv[1]) if len(sys.argv) > 1 else 3


def fragments(doc: bytes, msg_id: int, pieces: int):
    each = -(-len(doc) // pieces)
    for i in range(pieces):
        last = 0x80 if i == pieces - 1 else 0
        yield bytes([msg_id, last | i]) + doc[i * each:(i + 1) * each]


def log_after(ser, seconds=1.2):
    time.sleep(seconds)
    text = ser.read(ser.in_waiting or 1).decode(errors="replace")
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    if not lines:
        print("  (device logged nothing)")
    for line in lines:
        print("  device:", line)


async def main():
    ser = serial.Serial(PORT, 115200, timeout=0.2)
    print("scanning for HaloAI ...")
    dev = await BleakScanner.find_device_by_name("HaloAI", timeout=15.0)
    if dev is None:
        print("NOT FOUND: not advertising, or Bluetooth permission is denied")
        return 1

    async with BleakClient(dev) as client:
        print(f"connected, mtu {client.mtu_size}")

        settings = json.dumps({"phone_ready": True, "orientation": "landscape",
                               "time": "2026-10-16T18:00:00-05:00",
                               "tz": "America/Chicago"}).encode()
        print(f"\nsettings, {len(settings)} bytes, 1 fragment")
        ser.reset_input_buffer()
        await client.write_gatt_char(SETTINGS, next(fragments(settings, 1, 1)), response=True)
        log_after(ser)

        review = json.dumps({
            "sid": 42, "screen": "REVIEW", "rev": 3, "status_line": "Ready to confirm",
            "screen_lines": [f"line {chr(65 + i)} " + "x" * 38 for i in range(12)],
        }).encode()
        print(f"\nREVIEW screen, {len(review)} bytes, {PIECES} fragments")
        ser.reset_input_buffer()
        for f in fragments(review, 11, PIECES):
            await client.write_gatt_char(SCREEN, f, response=True)
        log_after(ser)
        print("  (a screen accepted in READY is logged at debug level, so silence here "
              "means it was NOT dropped)")

        # Positive proof that the whole message was reassembled and parsed: a
        # screen past the text budget is only detectable once the last fragment
        # has been joined on, and the device says so at error level.
        over = json.dumps({
            "sid": 43, "screen": "READ_ONLY_RESULT",
            "screen_lines": [f"row {i}" for i in range(30)],
        }).encode()
        print(f"\ntoo many lines, {len(over)} bytes, 4 fragments "
              f"(30 lines against a budget of 22, but under the 1 KB ceiling)")
        ser.reset_input_buffer()
        for f in fragments(over, 13, 4):
            await client.write_gatt_char(SCREEN, f, response=True)
        log_after(ser)
        print("  (this error can only be raised after the last fragment is joined on)")

        print(f"\nbroken sequence: fragment 1 skipped")
        ser.reset_input_buffer()
        parts = list(fragments(review, 12, 3))
        await client.write_gatt_char(SCREEN, parts[0], response=True)
        await client.write_gatt_char(SCREEN, parts[2], response=True)
        log_after(ser)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
