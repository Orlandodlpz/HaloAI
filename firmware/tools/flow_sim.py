#!/usr/bin/env python3
"""Walk a whole request to an approve, then power cycle the board and check that
the pending execution is reconciled from flash.

This is the BOOT / RECOVERY path, and it is the one thing on the device that
cannot be checked by reading the code: an action may already have taken effect in
the world, so the device has to come back up knowing about it rather than
returning to READY as if nothing had happened.

Needs the bench build, because it injects gestures over serial:

    idf.py -DHALO_BENCH_INJECT=1 build flash
    tools/flow_sim.py              # the outcome comes back as applied
    tools/flow_sim.py cancel       # the key turns out never to have been approved
    idf.py fullclean && idf.py build flash     # the flag is sticky, so clear it

The screen comes over BLE exactly as the phone would send it, prefix and all.
"""
import asyncio, json, sys, time

import serial
from bleak import BleakClient, BleakScanner

from serial_port import find_port

PORT = find_port()
BASE = "78450000-6bb6-46e1-be3c-499c2950b6ac"
SCREEN = BASE.replace("0000", "0004", 1)
SETTINGS = BASE.replace("0000", "0006", 1)
EVENTS = BASE.replace("0000", "0003", 1)

# A stand-in for the backend's 26 character execution key.
KEY = "01HQZX9K3M7N2P4R6T8V0W1Y3Z"

# "cancel": instead of telling the device the execution was applied, send a
# CANCELLED screen while it is reconciling, which is the case where the key was
# never approved and nothing happened.
CANCEL_MODE = len(sys.argv) > 1 and sys.argv[1] == "cancel"


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


def open_port():
    return serial.Serial(PORT, 115200, timeout=0.2)


def reboot(ser):
    """DTR drives IO0, RTS drives EN, through the usual transistor pair. DTR must
    be low or the chip comes up in the ROM loader instead of the app.

    Returns a FRESH port along with the boot log: on macOS the handle can go stale
    across a reset and the next use fails with "Device not configured", which
    reads like a dead board rather than a stale file descriptor."""
    ser.reset_input_buffer()
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    log = drain(ser, 8.0)
    ser.close()
    time.sleep(0.3)
    return open_port(), log


async def hello_state(ser):
    """Connect, read hello, disconnect. hello is the only place the device says
    which state it is in."""
    dev = await BleakScanner.find_device_by_name("HaloAI", timeout=15.0)
    if dev is None:
        return None, None
    got = []
    async with BleakClient(dev) as client:
        await client.start_notify(EVENTS, lambda _h, d: got.append(json.loads(bytes(d[2:]))))
        await asyncio.sleep(0.8)
    return (got[0] if got else None), dev


async def ensure_ready(ser):
    """Put the device back in READY whatever the last run left behind, so this
    tool can be run twice in a row. A run that died half way leaves a key in
    flash and the device in EXECUTION UNCERTAIN."""
    for _ in range(4):
        hello, _dev = await hello_state(ser)
        if hello is None:
            print("device not advertising")
            return None
        state = hello.get("state")
        if state == "READY":
            return hello
        print(f"  device is in {state}, recovering")
        ser.reset_input_buffer()
        if state == "EXECUTION_UNCERTAIN":
            ser.write(b"1")          # query the same key: RECONCILE STATUS
            time.sleep(0.5)
            ser.write(b"k")          # applied: clears the key
        elif state == "RECONCILE_STATUS":
            ser.write(b"k")
        elif state in ("SUCCESS", "FAILURE", "CANCELLED", "READ_ONLY_RESULT"):
            ser.write(b"1")
        else:
            ser.write(b"2")          # double press cancels anything pre-execution
        time.sleep(1.0)
        ser.write(b"1")              # dismiss whatever notice is showing
        time.sleep(1.5)
        drain(ser, 0.5)
    print("could not get the device back to READY")
    return None


async def main() -> int:
    ser = open_port()
    if await ensure_ready(ser) is None:
        return 1
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
    ser, boot = reboot(ser)
    show(boot)
    if "pending execution found in flash" not in boot:
        print("FAIL: the device came back up without reconciling")
        return 1
    print("  -> reconciled from flash")

    if CANCEL_MODE:
        # Carlos's team addition, 2026-10-17: the status endpoint says the key was
        # never approved and the cancel was confirmed, so the phone sends a
        # CANCELLED screen and the device drops the key.
        print("\nsending a CANCELLED screen: the key was never approved")
        dev2 = await BleakScanner.find_device_by_name("HaloAI", timeout=15.0)
        async with BleakClient(dev2) as c2:
            await asyncio.sleep(0.5)
            ser.reset_input_buffer()
            cancelled = json.dumps({"sid": 43, "screen": "CANCELLED",
                                    "status_line": "Cancelled"}).encode()
            await c2.write_gatt_char(SCREEN, one_fragment(cancelled, 30), response=True)
            show(drain(ser, 2.0))
    else:
        print("\ntelling the device the execution was applied")
        ser.reset_input_buffer()
        ser.write(b"k")
        show(drain(ser, 2.0))

    print("\npower cycling again, should be a clean boot")
    ser, boot2 = reboot(ser)
    show(boot2)
    if "pending execution found" in boot2:
        print("FAIL: a key is still in flash after the outcome was known")
        return 1
    print("  -> clean")

    print("\nBOOT / RECOVERY round trip OK")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
