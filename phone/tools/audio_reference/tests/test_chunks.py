import math
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import adpcm  # noqa: E402
import chunks  # noqa: E402


def test_crc_check_value():
    assert chunks.crc16_ccitt_false(b"123456789") == 0x29B1


def test_chunk_is_176_bytes_with_valid_crc():
    s = [int(5000 * math.sin(i / 10)) for i in range(640)]
    pkts = chunks.segment(s, req=1, rev=0, seg=0)
    assert [len(p) for p in pkts] == [176, 176, 16 + 6]
    for p in pkts:
        crc = struct.unpack("<H", p[10:12])[0]
        assert crc == chunks.crc16_ccitt_false(p[:10] + p[16:])


def test_final_marker_totals():
    pkts = chunks.segment([0] * 700, req=1, rev=0, seg=0)
    final = pkts[-1]
    assert final[1] & 1
    assert struct.unpack("<HI", final[16:]) == (3, 700)


def test_each_chunk_decodes_alone_and_tracks_the_input():
    s = [int(8000 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(3200)]
    pkts = chunks.segment(s, req=1, rev=0, seg=0)
    out = []
    for p in pkts[:-1][::-1]:  # decode in reverse order: no state carried between chunks
        h = chunks.HEADER.unpack(p[:16])
        out = adpcm.decode(p[16:], h[8], h[9]) + out
    err = sum((a - b) ** 2 for a, b in zip(s, out)) / len(s)
    sig = sum(a * a for a in s) / len(s)
    assert 10 * math.log10(sig / err) > 20  # ADPCM is lossy, but not that lossy
