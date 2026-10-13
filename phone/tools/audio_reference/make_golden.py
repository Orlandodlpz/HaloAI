"""Writes golden vectors shared with the Dart tests (phone/test/golden/):
the chunk bytes the Python encoder makes for a short signal, and the PCM the
reference decoder gets back. If Dart and Python ever disagree, the Dart test
fails before any radio is involved.

Run from phone/: python3 tools/audio_reference/make_golden.py
"""

import json
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(__file__))
import adpcm  # noqa: E402
import chunks  # noqa: E402

OUT = os.path.join(os.path.dirname(__file__), "..", "..", "test", "golden", "adpcm_chunks.json")


def signal():
    """0.5 s: a sweep, a loud burst that hits clipping, and silence, plus an
    odd sample count so the last chunk is partial."""
    rng = random.Random(7)
    s = []
    for i in range(8001):
        t = i / 16000
        v = 9000 * math.sin(2 * math.pi * (200 + 1800 * t) * t)
        if 3000 <= i < 3600:
            v = 40000 * (rng.random() - 0.5) * 2
        if i >= 6400:
            v = 0
        s.append(max(-32768, min(32767, int(v))))
    return s


def main():
    samples = signal()
    pkts = chunks.segment(samples, req=513, rev=2, seg=3)
    decoded = []
    for p in pkts[:-1]:
        h = chunks.HEADER.unpack(p[:16])
        decoded += adpcm.decode(p[16:], h[8], h[9])
    decoded = decoded[:len(samples)]
    with open(OUT, "w") as f:
        json.dump({
            "req": 513, "rev": 2, "seg": 3,
            "total_samples": len(samples),
            "chunks_hex": [p.hex() for p in pkts],
            "input_pcm": samples,
            "decoded_pcm": decoded,
        }, f)
    print(f"wrote {len(pkts)} packets, {len(samples)} samples to {os.path.normpath(OUT)}")


if __name__ == "__main__":
    main()
