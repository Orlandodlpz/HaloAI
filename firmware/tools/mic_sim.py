#!/usr/bin/env python3
"""Stand in for the phone while someone holds the button and SPEAKS.

phone_sim.py triggers the clip in flash and acknowledges it with the IDs that
clip was built with. A real recording is different in the ways that matter: the
IDs are whatever the device assigns, the chunks appear while the button is still
held rather than all at once, and the final marker only comes on release. This
is the tool for that, and it is the first thing in the project that exercises
capture at all.

    tools/mic_sim.py              # hold the button, speak, let go
    tools/mic_sim.py --seconds 30 # longer to get set up

Writes what it heard to received_mic.wav next to this file, so the test is
"play it back and listen", not "the counters look plausible".

The ADPCM decoder is a copy of the one in phone_sim.py, like every other bench
tool here: they are meant to run standalone with nothing but bleak and pyserial.
"""
import argparse, asyncio, json, os, struct, sys, time, wave

import serial
from bleak import BleakClient, BleakScanner

from serial_port import find_port

BASE = "78450000-6bb6-46e1-be3c-499c2950b6ac"
AUDIO = BASE.replace("0000", "0001", 1)
AUDIO_ACK = BASE.replace("0000", "0002", 1)
EVENTS = BASE.replace("0000", "0003", 1)
SETTINGS = BASE.replace("0000", "0006", 1)

HEADER = struct.Struct("<BBHBBHHHhBB")
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
]


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def decode(payload, predictor, index):
    out = []
    for byte in payload:
        for nibble in (byte & 0x0F, byte >> 4):      # low nibble first
            step = STEP_TABLE[index]
            diff = step >> 3
            if nibble & 4: diff += step
            if nibble & 2: diff += step >> 1
            if nibble & 1: diff += step >> 2
            predictor = clamp(predictor - diff if nibble & 8 else predictor + diff, -32768, 32767)
            index = clamp(index + INDEX_TABLE[nibble], 0, 88)
            out.append(predictor)
    return out


class Rx:
    def __init__(self):
        self.chunks, self.final = {}, None
        self.req = self.seg = None
        self.bytes = self.received = self.bad_crc = 0
        self.first_at = self.last_at = None

    def upto(self):
        n = -1
        while (n + 1) in self.chunks:
            n += 1
        return n

    def missing_from(self):
        u = self.upto()
        if self.chunks and max(self.chunks) > u + 1:
            return u + 1
        if self.final is not None and (u + 1) < self.final[1]:
            return u + 1
        return None


async def main():
    p = argparse.ArgumentParser()
    p.add_argument("--seconds", type=int, default=45, help="how long to wait for a recording")
    args = p.parse_args()

    ser = serial.Serial(find_port(), 115200, timeout=0.1)
    print("scanning for HaloAI ...")
    dev = await BleakScanner.find_device_by_name("HaloAI", timeout=15.0)
    if dev is None:
        print("NOT FOUND: not advertising, or Bluetooth permission is denied")
        return 1

    rx = Rx()
    done = asyncio.Event()

    async with BleakClient(dev) as client:
        print(f"connected, mtu {client.mtu_size}")

        def on_audio(_h, data: bytearray):
            now = time.perf_counter()
            if rx.first_at is None:
                rx.first_at = now
            rx.last_at = now
            rx.received += 1
            rx.bytes += len(data)
            fmt, flags, req, rev, seg, seq, plen, crc, pred, step, _ = HEADER.unpack(data[:16])
            payload = bytes(data[16:16 + plen])
            if crc16(bytes(data[:10]) + payload) != crc:
                rx.bad_crc += 1
                return
            # The IDs come from the device, not from us: this is a recording it
            # started, not a clip we asked for.
            rx.req, rx.seg = req, seg
            if flags & 1:
                total_chunks, total_samples = struct.unpack("<HI", payload)
                rx.final = (seq, total_chunks, total_samples)
                print(f"  final marker: {total_chunks} chunks, {total_samples} samples "
                      f"({total_samples / 16000:.2f} s)")
            else:
                rx.chunks[seq] = (payload, pred, step)
                if len(rx.chunks) % 25 == 0:
                    print(f"  {len(rx.chunks)} chunks ({len(rx.chunks) * 0.02:.1f} s of audio)")

        def on_events(_h, data: bytearray):
            try:
                print("event:", json.loads(bytes(data[2:]).decode()))
            except Exception:
                pass

        await client.start_notify(AUDIO, on_audio)    # audio first
        await client.start_notify(EVENTS, on_events)  # events last: this triggers hello
        await asyncio.sleep(0.4)

        # Without phone_ready a hold stops at PHONE / BLUETOOTH CHECK and never
        # reaches RECORDING.
        await client.write_gatt_char(SETTINGS, bytes([1, 0x80]) + json.dumps(
            {"phone_ready": True, "orientation": "landscape"}).encode(), response=True)

        async def acker():
            last = 0.0
            while not done.is_set():
                await asyncio.sleep(0.05)
                if rx.req is None:
                    continue
                u = rx.upto()
                have_all = rx.final is not None and u + 1 >= rx.final[1]
                gap = rx.missing_from()
                base = {"req": rx.req, "seg": rx.seg}
                if have_all:
                    await client.write_gatt_char(AUDIO_ACK, json.dumps(
                        {**base, "upto": u, "final": True}).encode(), response=False)
                    done.set()
                    return
                now = time.perf_counter()
                if u >= 0 and now - last > 0.2:
                    msg = {**base, "upto": u}
                    if gap is not None:
                        msg["resend_from"] = gap
                    await client.write_gatt_char(AUDIO_ACK, json.dumps(msg).encode(),
                                                 response=False)
                    last = now

        task = asyncio.create_task(acker())
        # MENU, not the side button: on the development board MENU (IO2) is what
        # stands in for the badge's side button. kBtnSide on IO15 is a breadboard
        # button that may not be wired, and a hold there reaches nothing.
        print(f"\n>>> HOLD THE *MENU* BUTTON, SAY SOMETHING, THEN LET GO. <<<")
        print(f">>> Waiting up to {args.seconds} s.\n")
        try:
            await asyncio.wait_for(done.wait(), timeout=args.seconds)
        except asyncio.TimeoutError:
            print("TIMEOUT: no complete segment")
        task.cancel()

        elapsed = (rx.last_at - rx.first_at) if rx.first_at and rx.last_at else 0
        print("\n--- link report ---")
        print(f"chunks stored      {len(rx.chunks)}")
        print(f"bytes over the air {rx.bytes}")
        print(f"bad CRC            {rx.bad_crc}")
        print(f"final marker       {rx.final}")
        if elapsed > 0:
            print(f"wall time          {elapsed:.2f} s")
            print(f"throughput         {rx.bytes * 8 / elapsed / 1000:.0f} kbit/s")

        if rx.final and len(rx.chunks) >= rx.final[1]:
            pcm = []
            for i in range(rx.final[1]):
                payload, pred, step = rx.chunks[i]
                pcm.extend(decode(payload, pred, step))
            pcm = pcm[:rx.final[2]]
            peak = max((abs(v) for v in pcm), default=0)
            out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "received_mic.wav")
            with wave.open(out, "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
                w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
            print(f"peak sample        {peak} ({'silence!' if peak < 200 else 'audible'})")
            print(f"\nwrote {out}, {len(pcm)} samples. Play it and listen.")
            ser.close()
            return 0 if peak >= 200 else 1
    ser.close()
    return 1


sys.exit(asyncio.run(main()))
