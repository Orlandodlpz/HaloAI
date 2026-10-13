"""Audio chunk layout from BRIEFING section 2: 16 byte little endian header,
then the payload. Mirrors phone/lib/ble/audio_chunk.dart."""

import struct

from adpcm import Encoder

FORMAT_IMA_ADPCM_16K_MONO = 1
SAMPLES_PER_CHUNK = 320  # 20 ms at 16 kHz
HEADER = struct.Struct("<BBHBBHHHhBB")  # format flags req rev seg seq len crc pred step reserved
assert HEADER.size == 16


def crc16_ccitt_false(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build(req, rev, seg, seq, payload, predictor=0, step_index=0, final=False):
    flags = 1 if final else 0
    head = HEADER.pack(FORMAT_IMA_ADPCM_16K_MONO, flags, req, rev, seg, seq, len(payload),
                       0, predictor, step_index, 0)
    crc = crc16_ccitt_false(head[:10] + payload)
    return head[:10] + struct.pack("<H", crc) + head[12:] + payload


def segment(samples, req, rev, seg):
    """Splits int16 samples into audio chunks plus the final marker.
    The last chunk is zero padded to an even sample count; total samples in
    the final marker tells the phone where the real audio ends."""
    enc = Encoder()
    out = []
    seq = 0
    for start in range(0, len(samples), SAMPLES_PER_CHUNK):
        part = list(samples[start:start + SAMPLES_PER_CHUNK])
        if len(part) % 2:
            part.append(0)
        pred, idx = enc.predictor, enc.index
        out.append(build(req, rev, seg, seq, enc.encode(part), pred, idx))
        seq += 1
    out.append(build(req, rev, seg, seq, struct.pack("<HI", seq, len(samples)), final=True))
    return out
