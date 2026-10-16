#!/usr/bin/env python3
"""Walk a whole request to an approve, then power cycle the board and check that
the pending execution is reconciled from flash.

This is the BOOT / RECOVERY path, and it is the one thing on the device that
cannot be checked by reading the code: an action may already have taken effect in
the world, so the device has to come back up knowing about it rather than
returning to READY as if nothing had happened.

Needs the bench build, because it injects gestures over serial:

    idf.py -DHALO_BENCH_INJECT=1 build flash
    tools/flow_sim.py
    idf.py fullclean && idf.py build flash     # the flag is sticky, so clear it

The screen comes over BLE exactly as the phone would send it, prefix and all.
"""
import asyncio, json, sys, time

import serial
from bleak import BleakClient, BleakScanner

PORT = "/dev/cu.usbserial-110"
BASE = "78450000-6bb6-46e1-be3c-499c2950b6ac"
SCREEN = BASE.replace("0000", "0004", 1)
SETTINGS = BASE.replace("0000", "0006", 1)
EVENTS = BASE.replace("0000", "0003", 1)

# A stand-in for the backend's 26 character execution key.
KEY = "01HQZX9K3M7N2P4R6T8V0W1Y3Z"


def one_fragment(doc: bytes, msg_id: int) -> bytes:
    """A whole message in one write: message ID, then the last-fragment flag."""
    return bytes([msg_id, 0x80]) + doc


def drain(ser, seconds: float) -> str:
    """Collect the device log. in_waiting has to be polled: one read after a
    sleep silently truncates a long boot log."""
    buf, end = "", time.time() + seconds
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf += ser.read(n).decode(errors="replace")
        time.sleep(0.05)
    return buf


def show(text: str, keep=("halo:",), skip=("RAW",)) -> None:
    for line in text.splitlines():
        if any(k in line for k in keep) and not any(s in line for s in skip):
            print("   ", line.strip())


def reboot(ser) -> str:
    """DTR drives IO0, RTS drives EN, through the usual transistor pair. DTR must
    be low or the chip comes up in the ROM loader instead of the app."""
    ser.reset_input_buffer()
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    return drain(ser, 8.0)


async def main() -> int:
    ser = serial.Serial(PORT, 115200, timeout=0.2)
    dev = await BleakScanner.find_device_by_name("HaloAI", timeout=15.0)
    if dev is None:
        print("NOT FOUND: the device is not advertising")
        return 1

    async with BleakClient(dev) as client:
        events = []
        await client.start_notify(EVENTS, lambda _h, d: events.append(json.loads(bytes(d[2:]))))
        await asyncio.sleep(0.6)
        print("hello:", events[0] if events else "(none)")

        ser.reset_input_buffer()
        await client.write_gatt_char(
            SETTINGS, one_fragment(json.dumps({"phone_ready": True}).encode(), 1), response=True)
        drain(ser, 0.8)

        print("\nwalking hold -> phone ready -> release -> audio acked")
        ser.reset_input_buffer()
        for key in (b"h", b"p", b"R", b"a"):
            ser.write(key)
            time.sleep(0.4)
        show(drain(ser, 0.8))

        review = json.dumps({
            "sid": 42, "screen": "REVIEW", "rev": 3, "pending_key": KEY,
            "status_line": "Ready to confirm",
            "screen_lines": ["To: victor@example.com", "Subject: Meeting moved to Thursday"],
        }).encode()
        print("\nREVIEW screen, carrying the execution key")
        ser.reset_input_buffer()
        await client.write_gatt_char(SCREEN, one_fragment(review, 20), response=True)
        show(drain(ser, 2.0))

        print("\nsingle press, after the 1 s confirm guard")
        ser.reset_input_buffer()
        ser.write(b"1")
        approved = drain(ser, 2.0)
        show(approved)
        if "execution key saved" not in approved:
            print("FAIL: the key was never written to flash")
            return 1

    print("\npower cycling the board")
    boot = reboot(ser)
    show(boot)
    if "pending execution found in flash" not in boot:
        print("FAIL: the device came back up without reconciling")
        return 1
    print("  -> reconciled from flash")

    print("\ntelling the device the execution was applied")
    ser.reset_input_buffer()
    ser.write(b"k")
    show(drain(ser, 2.0))

    print("\npower cycling again, should be a clean boot")
    boot2 = reboot(ser)
    show(boot2)
    if "pending execution found" in boot2:
        print("FAIL: a key is still in flash after the outcome was known")
        return 1
    print("  -> clean")

    print("\nBOOT / RECOVERY round trip OK")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
