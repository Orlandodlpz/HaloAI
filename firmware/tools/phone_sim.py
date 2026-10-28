#!/usr/bin/env python3
"""Stand in for Carlos's phone: connect to the device over BLE, receive the audio
segment, acknowledge it the way the contract says, and report what the link did.

This is NOT a replacement for testing against the real phone. It speaks the same
contract, so it measures the device side of the link honestly, but Android's BLE
stack is not CoreBluetooth and the throughput numbers will differ.
"""
import asyncio, json, os, struct, sys, time, wave

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

# --live N records from the real microphone for N seconds instead of streaming
# the clip embedded in flash. It needs the bench build, because it injects the
# hold and the release over serial. This is the only way to exercise capture and
# the link together: the flash clip proves the link alone.
LIVE_SECONDS = 0.0
SEND_REVIEW = False
_args = []
for _a in sys.argv[1:]:
    if _a.startswith("--live"):
        LIVE_SECONDS = float(_a.split("=", 1)[1]) if "=" in _a else 3.0
    elif _a == "--review":
        SEND_REVIEW = True
    else:
        _args.append(_a)
sys.argv = [sys.argv[0]] + _args

DROP_EVERY = int(sys.argv[1]) if len(sys.argv) > 1 else 0  # 0 = drop nothing
# Pretend the first N final markers never arrived. The device must resend the
# marker every 300 ms until it is acknowledged; without that the phone waits for
# an end it is never told about and the whole run hangs.
DROP_FINALS = int(sys.argv[2]) if len(sys.argv) > 2 else 0


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
        self.finals_seen = 0
        self.finals_dropped = 0

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
    ser = serial.Serial(find_port(), 115200, timeout=0.1)
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
                rx.finals_seen += 1
                if rx.finals_dropped < DROP_FINALS:
                    rx.finals_dropped += 1
                    print(f"dropping final marker #{rx.finals_seen} on purpose; "
                          f"the device should resend it")
                    return
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
        what = []
        if DROP_EVERY: what.append(f"dropping every {DROP_EVERY}th chunk")
        if DROP_FINALS: what.append(f"dropping the first {DROP_FINALS} final marker(s)")
        print("triggering the stream" + (" (" + ", ".join(what) + ") on purpose" if what else ""))
        if LIVE_SECONDS > 0:
            # The device asks whether the phone is there before it records, and
            # answers "phone absent" if nothing has said so. The real phone sends
            # this on connect; the 'A' shortcut never needed it.
            await client.write_gatt_char(
                SETTINGS,
                bytes([1, 0x80]) + json.dumps(
                    {"phone_ready": True, "orientation": "landscape"}).encode(),
                response=True)
            await asyncio.sleep(0.3)
            print(f"LIVE: holding the button for {LIVE_SECONDS:.1f} s. SPEAK AT THE BADGE NOW.")
            # Wake first. A hold in WALLPAPER wakes the screen and is consumed,
            # by design, so it does not start a recording. Then hold, then
            # phone_ready: a hold alone stops in PHONE / BLUETOOTH CHECK waiting
            # for the phone to say it is there, and nothing is captured. The 'A'
            # shortcut skips the state machine; this path does not.
            # Long enough for whatever the last run left behind to settle back
            # to READY. A double press in REVIEW cancels, and CANCELLED takes its
            # own outcome timeout to return, so a short wait here puts the hold
            # into a state that ignores it and nothing records.
            ser.write(b"2")
            ser.flush()
            await asyncio.sleep(4.0)
            ser.write(b"h")
            ser.flush()
            await asyncio.sleep(0.3)
            ser.write(b"p")
            ser.flush()
            await asyncio.sleep(LIVE_SECONDS)
            ser.write(b"R")
            print("released")
        else:
            ser.write(b"A")
        try:
            await asyncio.wait_for(done.wait(), timeout=60)
        except asyncio.TimeoutError:
            print("TIMEOUT waiting for the segment")
        task.cancel()

        # What the device made of it. The link report says the bytes arrived;
        # this says whether the state machine agreed the segment was delivered,
        # which is a different question and the one that was wrong on 2026-10-28.
        #
        # --review is the sharp version of that question. A REVIEW screen is only
        # accepted from PROCESSING, so if the device is still sitting in
        # FINALIZE AUDIO it answers "ignored in FINALIZE AUDIO" and the audio
        # never completed. Accepted means the whole path worked.
        if SEND_REVIEW:
            await asyncio.sleep(1.0)
            doc = json.dumps({
                "sid": 1, "screen": "REVIEW", "status_line": "Ready to confirm",
                "screen_lines": ["Create event", "", "Gym",
                                 "Tomorrow, Tue 28 Oct, 3:00 - 4:00 PM", "",
                                 "Press to confirm, double press to cancel."],
            }).encode()
            screen_uuid = BASE.replace("0000", "0004", 1)
            for i in range(0, len(doc), 180):
                last = 0x80 if i + 180 >= len(doc) else 0
                await client.write_gatt_char(
                    screen_uuid,
                    bytes([7, last | (i // 180)]) + doc[i:i + 180], response=True)

        # Poll rather than one read: a single read after a sleep returns almost
        # nothing on this link and the interesting lines are the last ones.
        tail, _end = "", time.time() + 3.0
        while time.time() < _end:
            n = ser.in_waiting
            if n:
                tail += ser.read(n).decode(errors="replace")
            else:
                await asyncio.sleep(0.05)
        # Transitions, plus the two lines that say when the audio actually left:
        # "recorded ..." is the final encode and "final marker sent" is the last
        # thing the phone is waiting for. Release to marker is the per command
        # cost of drawing a screen before sending, which is what I4 measures.
        moved = [l.strip() for l in tail.splitlines()
                 if "->" in l or "ignored" in l
                 or "final marker sent" in l or "recorded " in l
                 or "panel: present" in l]
        if moved:
            print("\n--- what the device did ---")
            for l in moved[-9:]:
                print("  ", l.split("halo: ")[-1][:110])

        elapsed = (rx.last_at - rx.first_at) if rx.first_at and rx.last_at else 0
        print("\n--- link report ---")
        print(f"chunks stored      {len(rx.chunks)}")
        print(f"notifications      {rx.received}")
        print(f"bytes over the air {rx.bytes}")
        print(f"deliberately lost  {rx.dropped}")
        print(f"bad CRC            {rx.bad_crc}")
        print(f"final marker       {rx.final}")
        print(f"final markers seen {rx.finals_seen} ({rx.finals_dropped} dropped on purpose)")
        if elapsed > 0:
            print(f"wall time          {elapsed:.2f} s")
            print(f"throughput         {rx.bytes * 8 / elapsed / 1000:.0f} kbit/s")
        if rx.final:
            audio_s = rx.final[2] / 16000
            print(f"audio length       {audio_s:.2f} s")
            if elapsed > 0:
                print(f"realtime factor    {audio_s / elapsed:.2f}x "
                      f"({'keeps up' if audio_s / elapsed >= 1 else 'TOO SLOW to stream live'})"
                      if LIVE_SECONDS == 0 else
                      "(live capture: the audio is produced as it is sent, so this "
                      "is about 1.0x by definition and says nothing about headroom. "
                      "Use the flash clip for that.)")

        if rx.final and len(rx.chunks) == rx.final[1]:
            pcm = []
            for i in range(rx.final[1]):
                payload, pred, step = rx.chunks[i]
                pcm.extend(decode(payload, pred, step))
            pcm = pcm[:rx.final[2]]
            # Next to this tool, not an absolute path. This was hardcoded to a
            # scratch directory from the session that wrote it, so every later
            # run quietly dropped its recording somewhere nobody would look.
            out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "received.wav")
            with wave.open(out_path, "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
                w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
            print(f"wrote {out_path}, {len(pcm)} samples")
    ser.close()
    return 0


sys.exit(asyncio.run(main()))
