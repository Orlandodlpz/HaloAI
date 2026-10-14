#!/usr/bin/env python3
"""Stand in for Carlos's phone: connect to the device over BLE, receive the audio
segment, acknowledge it the way the contract says, and report what the link did.

This is NOT a replacement for testing against the real phone. It speaks the same
contract, so it measures the device side of the link honestly, but Android's BLE
stack is not CoreBluetooth and the throughput numbers will differ.
"""
import asyncio, json, struct, sys, time, wave

import serial
from bleak import BleakClient, BleakScanner

BASE = "78450000-6bb6-46e1-be3c-499c2950b6ac"
AUDIO = BASE.replace("0000", "0001", 1)
AUDIO_ACK = BASE.replace("0000", "0002", 1)
EVENTS = BASE.replace("0000", "0003", 1)

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

DROP_EVERY = int(sys.argv[1]) if len(sys.argv) > 1 else 0  # 0 = drop nothing


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
        for nib in (byte & 0x0F, byte >> 4):
            step = STEP_TABLE[index]
            diff = step >> 3
            if nib & 4: diff += step
            if nib & 2: diff += step >> 1
            if nib & 1: diff += step >> 2
            predictor = clamp(predictor - diff if nib & 8 else predictor + diff, -32768, 32767)
            index = clamp(index + INDEX_TABLE[nib], 0, 88)
            out.append(predictor)
    return out


class Rx:
    def __init__(self):
        self.chunks = {}
        self.final = None
        self.bad_crc = 0
        self.dropped = 0
        self.first_at = None
        self.last_at = None
        self.bytes = 0
        self.received = 0
        self.dropped_once = set()  # drop each sequence number once, not forever

    def upto(self):
        n = -1
        while (n + 1) in self.chunks:
            n += 1
        return n

    def missing_from(self):
        # A gap is reported as soon as it appears, not only once the final marker
        # arrives: something past the hole has been stored, so the hole is real.
        u = self.upto()
        if self.chunks and max(self.chunks) > u + 1:
            return u + 1
        if self.final is not None and (u + 1) < self.final[1]:
            return u + 1
        return None


async def main():
    ser = serial.Serial("/dev/cu.usbserial-110", 115200, timeout=0.1)
    print("scanning for HaloAI ...")
    dev = await BleakScanner.find_device_by_name("HaloAI", timeout=15.0)
    if dev is None:
        print("NOT FOUND: the device is not advertising, or Bluetooth permission is denied")
        return 1
    print(f"found {dev.address}")

    rx = Rx()
    done = asyncio.Event()

    async with BleakClient(dev) as client:
        print(f"connected, mtu {client.mtu_size}")
        if client.mtu_size < 179:
            print(f"WARNING: mtu {client.mtu_size} is below the 179 a 176 byte chunk needs")

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
            if (DROP_EVERY and not (flags & 1) and seq % DROP_EVERY == DROP_EVERY - 1
                    and seq not in rx.dropped_once):
                rx.dropped_once.add(seq)
                rx.dropped += 1
                return  # pretend this one never arrived, but let the resend through
            if flags & 1:
                total_chunks, total_samples = struct.unpack("<HI", payload)
                rx.final = (seq, total_chunks, total_samples)
            else:
                rx.chunks[seq] = (payload, pred, step)

        def on_events(_h, data: bytearray):
            # 2 byte prefix: message id, then last-flag plus fragment index.
            body = bytes(data[2:])
            try:
                print("event:", json.loads(body.decode()))
            except Exception:
                print("event (fragment):", body[:40])

        await client.start_notify(AUDIO, on_audio)   # audio first
        await client.start_notify(EVENTS, on_events)  # events last, which triggers hello
        await asyncio.sleep(0.5)

        async def acker():
            last = 0.0
            while not done.is_set():
                await asyncio.sleep(0.05)
                u = rx.upto()
                have_all = rx.final is not None and u + 1 >= rx.final[1]
                gap = rx.missing_from()
                if have_all:
                    await client.write_gatt_char(
                        AUDIO_ACK, json.dumps({"req": 513, "seg": 3, "upto": u,
                                               "final": True}).encode(), response=False)
                    done.set()
                    return
                now = time.perf_counter()
                if u >= 0 and (now - last > 0.2):
                    msg = {"req": 513, "seg": 3, "upto": u}
                    if gap is not None and gap < 10000:
                        msg["resend_from"] = gap
                    await client.write_gatt_char(AUDIO_ACK, json.dumps(msg).encode(),
                                                 response=False)
                    last = now

        task = asyncio.create_task(acker())
        print(f"triggering the stream (dropping every {DROP_EVERY}th chunk on purpose)"
              if DROP_EVERY else "triggering the stream")
        ser.write(b"A")
        try:
            await asyncio.wait_for(done.wait(), timeout=60)
        except asyncio.TimeoutError:
            print("TIMEOUT waiting for the segment")
        task.cancel()

        elapsed = (rx.last_at - rx.first_at) if rx.first_at and rx.last_at else 0
        print("\n--- link report ---")
        print(f"chunks stored      {len(rx.chunks)}")
        print(f"notifications      {rx.received}")
        print(f"bytes over the air {rx.bytes}")
        print(f"deliberately lost  {rx.dropped}")
        print(f"bad CRC            {rx.bad_crc}")
        print(f"final marker       {rx.final}")
        if elapsed > 0:
            print(f"wall time          {elapsed:.2f} s")
            print(f"throughput         {rx.bytes * 8 / elapsed / 1000:.0f} kbit/s")
        if rx.final:
            audio_s = rx.final[2] / 16000
            print(f"audio length       {audio_s:.2f} s")
            if elapsed > 0:
                print(f"realtime factor    {audio_s / elapsed:.2f}x "
                      f"({'keeps up' if audio_s / elapsed >= 1 else 'TOO SLOW to stream live'})")

        if rx.final and len(rx.chunks) == rx.final[1]:
            pcm = []
            for i in range(rx.final[1]):
                payload, pred, step = rx.chunks[i]
                pcm.extend(decode(payload, pred, step))
            pcm = pcm[:rx.final[2]]
            with wave.open("/Users/snpr/.claude/jobs/afcf87b7/tmp/received.wav", "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
                w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
            print(f"wrote received.wav, {len(pcm)} samples")
    ser.close()
    return 0


sys.exit(asyncio.run(main()))
