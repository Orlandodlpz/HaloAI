#!/usr/bin/env python3
"""Send a wallpaper to the device the way the phone does, and say what came back.

The format is Carlos's P4 proposal, answered in firmware/docs/ble_fields.md: one
JSON write per chunk on the Wallpaper characteristic, each with the usual 2 byte
fragment prefix, `off` continuing where the last one ended, and `crc` on the last
one marking the end. The device answers once, on Events.

The image is the badge's whole canvas, 800 x 480 landscape or 480 x 800 portrait,
1 bit per pixel, most significant bit leftmost, **1 means white** - the same bytes
the phone's dither.dart produces. The development panel shows 792 x 272 of it and
squeezes the rest, which is the point of the test pattern: it is readable squeezed.

    tools/wallpaper_sim.py                 # the test pattern, landscape
    tools/wallpaper_sim.py --portrait
    tools/wallpaper_sim.py --drop 7        # drop every 7th chunk: exercises resend_from
    tools/wallpaper_sim.py --break-crc     # corrupt a byte: the device must refuse and restart
    tools/wallpaper_sim.py --image me.jpg  # a real photo, needs Pillow
    tools/wallpaper_sim.py --raw shot.bin  # exactly 48000 bytes, already packed
"""
import argparse, asyncio, base64, json, sys, time

import serial
from bleak import BleakClient, BleakScanner

from serial_port import find_port

BASE = "78450000-6bb6-46e1-be3c-499c2950b6ac"
EVENTS = BASE.replace("0000", "0003", 1)
WALLPAPER = BASE.replace("0000", "0005", 1)
SETTINGS = BASE.replace("0000", "0006", 1)

IMAGE_BYTES = 48000


# --------------------------------------------------------------- the picture

def test_pattern(w: int, h: int) -> bytes:
    """A pattern built to be judged by eye on a panel that squeezes it: a frame,
    corner blocks that name their corner, a density ramp and a gray wedge.

    Pure Python so the tool has no dependency the other tools do not already
    have. 384000 pixels takes a second or two; that is fine for a bench tool."""
    gray = bytearray(w * h)
    for y in range(h):
        row = y * w
        for x in range(w):
            v = 255 * x // (w - 1)                       # left to right ramp
            if y < h // 3:
                v = 255 if ((x // 16) + (y // 16)) % 2 else 0   # checkerboard
            elif y > 2 * h // 3:
                period = 2 + (x * 14) // w               # bars, coarse to fine
                v = 255 if (x // period) % 2 else 0
            gray[row + x] = v

    def block(x0, y0, size, value):
        for y in range(y0, min(y0 + size, h)):
            for x in range(x0, min(x0 + size, w)):
                gray[y * w + x] = value

    m = 24
    block(m, m, 48, 0)                       # every corner gets a black square,
    block(w - m - 48, m, 48, 0)              # so a corner that does not reach the
    block(m, h - m - 48, 48, 0)              # glass can be named rather than
    block(w - m - 48, h - m - 48, 48, 0)     # described
    for x in range(w):                       # a 2 px frame
        for y in (0, 1, h - 2, h - 1):
            gray[y * w + x] = 0
    for y in range(h):
        for x in (0, 1, w - 2, w - 1):
            gray[y * w + x] = 0
    return floyd_steinberg(gray, w, h)


def floyd_steinberg(gray: bytearray, w: int, h: int) -> bytes:
    """8 bit gray in, packed 1 bit out, 1 = white. The same error diffusion and
    the same packing as phone/lib/wallpaper/dither.dart, so the device sees the
    kind of bytes the phone really sends."""
    err = [float(v) for v in gray]
    out = bytearray(w * h // 8)
    for y in range(h):
        row = y * w
        for x in range(w):
            i = row + x
            old = err[i]
            white = old >= 128
            if white:
                out[i >> 3] |= 0x80 >> (i & 7)
            e = old - (255 if white else 0)
            if x + 1 < w:
                err[i + 1] += e * 7 / 16
            if y + 1 < h:
                if x > 0:
                    err[i + w - 1] += e * 3 / 16
                err[i + w] += e * 5 / 16
                if x + 1 < w:
                    err[i + w + 1] += e * 1 / 16
    return bytes(out)


def from_photo(path: str, w: int, h: int) -> bytes:
    from PIL import Image, ImageOps   # only this path needs Pillow
    im = ImageOps.exif_transpose(Image.open(path)).convert("L")
    im = ImageOps.fit(im, (w, h), method=Image.LANCZOS)   # centre crop, like the app
    return floyd_steinberg(bytearray(im.tobytes()), w, h)


# ----------------------------------------------------------------- the wire

def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE, the same one the audio chunks use."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def bytes_per_write(max_write: int) -> int:
    overhead = len('{"wp":255,"w":800,"h":800,"off":48000,"data":"","crc":65535}')
    return ((max_write - 2 - overhead) // 4) * 3


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--portrait", action="store_true")
    p.add_argument("--drop", type=int, default=0, help="drop every Nth chunk on purpose")
    p.add_argument("--break-crc", action="store_true", help="corrupt one byte after the CRC")
    p.add_argument("--image", help="a photo, needs Pillow")
    p.add_argument("--raw", help="a file of exactly 48000 packed bytes")
    p.add_argument("--wp", type=int, default=7, help="transfer ID")
    p.add_argument("--pace-ms", type=float, default=10.0,
                   help="wait between writes. The default is the rate this Mac can "
                        "deliver without losing packets; CoreBluetooth drops writes "
                        "without response when its own queue fills and tells nobody. "
                        "The board accepted 109 kbit/s with zero loss and showed gaps "
                        "above that, all of them the sender's.")
    p.add_argument("--attempts", type=int, default=3, help="what the phone allows itself")
    args = p.parse_args()

    w, h = (480, 800) if args.portrait else (800, 480)
    if args.raw:
        bits = open(args.raw, "rb").read()
        if len(bits) != IMAGE_BYTES:
            print(f"{args.raw} is {len(bits)} bytes, not {IMAGE_BYTES}")
            return 1
    elif args.image:
        print(f"preparing {args.image} at {w} x {h} ...")
        bits = from_photo(args.image, w, h)
    else:
        print(f"drawing the test pattern at {w} x {h} ...")
        bits = test_pattern(w, h)

    crc = crc16(bits)
    if args.break_crc:
        bits = bytes([bits[0] ^ 0xFF]) + bits[1:]   # the CRC now describes the old bytes
    return asyncio.run(send(bits, w, h, crc, args))


async def send(bits: bytes, w: int, h: int, crc: int, args) -> int:
    ser = serial.Serial(find_port(), 115200, timeout=0.2)
    print("scanning for HaloAI ...")
    dev = await BleakScanner.find_device_by_name("HaloAI", timeout=15.0)
    if dev is None:
        print("NOT FOUND: not advertising, or Bluetooth permission is denied")
        return 1

    answers = []
    last = None

    def on_event(_h, data: bytes):
        try:
            msg = json.loads(data[2:].decode())       # strip the fragment prefix
        except Exception:
            return
        if msg.get("type") == "wallpaper":
            answers.append(msg)
            print("  device answered:", json.dumps(msg))

    async with BleakClient(dev) as client:
        print(f"connected, mtu {client.mtu_size}")
        await client.start_notify(EVENTS, on_event)
        orientation = "portrait" if args.portrait else "landscape"
        await client.write_gatt_char(
            SETTINGS, bytes([1, 0x80]) + json.dumps({"orientation": orientation}).encode(),
            response=True)

        per = bytes_per_write(client.mtu_size - 3)
        total = -(-len(bits) // per)
        print(f"{len(bits)} bytes, {per} per write, {total} writes, crc {crc:04x}")
        if args.drop:
            print(f"dropping every {args.drop}th chunk on purpose, first attempt only")

        # Pace the writes. A write without response returns the moment
        # CoreBluetooth has queued it, so without this the whole image goes out in
        # 30 ms, which no phone does and the board cannot drain: it answers
        # resend_from and the attempt is wasted. Android paces itself on the
        # controller's buffers; this is the stand in for that.
        ser.reset_input_buffer()
        msg_id = 0
        frm = 0
        last = None
        for attempt in range(1, args.attempts + 1):
            answers.clear()
            started = time.time()
            sent = dropped = 0
            for n, off in enumerate(range(frm, len(bits), per)):
                part = bits[off:off + per]
                if attempt == 1 and args.drop and (n + 1) % args.drop == 0:
                    dropped += 1
                    continue
                body = {"wp": args.wp, "w": w, "h": h, "off": off,
                        "data": base64.b64encode(part).decode()}
                if off + len(part) == len(bits):
                    body["crc"] = crc
                doc = json.dumps(body, separators=(",", ":")).encode()
                msg_id = (msg_id + 1) & 0xFF
                await client.write_gatt_char(WALLPAPER, bytes([msg_id, 0x80]) + doc,
                                             response=False)
                sent += 1
                if args.pace_ms:
                    await asyncio.sleep(args.pace_ms / 1000)
            took = time.time() - started
            rate = (sent * per * 8) / took / 1000 if took else 0
            print(f"attempt {attempt}: {sent} writes from byte {frm} in {took:.2f} s "
                  f"({rate:.0f} kbit/s)"
                  f"{f', {dropped} dropped on purpose' if dropped else ''}")

            for _ in range(100):          # 10 s, the phone's own timeout
                if answers:
                    break
                await asyncio.sleep(0.1)
            if not answers:
                print("  no answer")
                break
            last = answers[-1]
            if last.get("ok"):
                break
            resend = last.get("resend_from")
            if resend is None:
                print("  refused for good, not retrying")
                break
            frm = resend - resend % per   # whole chunks, the way the phone resends

        # The answer comes after the last chunk, and a resend request can come
        # sooner. Ten seconds is the phone's own timeout.
        for _ in range(100):
            if answers:
                break
            await asyncio.sleep(0.1)
        await asyncio.sleep(0.3)
        await client.stop_notify(EVENTS)

    # Poll in a loop: one read after a sleep truncates the log silently and
    # looks like a board that said nothing.
    buf = b""
    until = time.time() + 2.0
    while time.time() < until:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            until = time.time() + 0.5
        time.sleep(0.05)
    log = buf.decode(errors="replace")
    for line in (l.strip() for l in log.splitlines()):
        if line:
            print("  device:", line)

    if last is None:
        print("\nFAIL: the device never answered")
        return 1
    if args.break_crc:
        # The CRC describes bytes that were never sent, so every attempt must be
        # refused. Storing it would be the real failure.
        print("\nOK: refused, nothing stored" if not last.get("ok") else
              "\nFAIL: a wrong CRC was accepted")
        return 0 if not last.get("ok") else 1
    print("\nOK: wallpaper stored" if last.get("ok") else "\nFAIL: " + json.dumps(last))
    return 0 if last.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
