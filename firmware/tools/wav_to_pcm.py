#!/usr/bin/env python3
"""Extract raw 16 kHz mono int16 samples from a WAV, for embedding in the firmware.

The source of truth is the committed WAV in phone/assets/clips/. Regenerate with:
  python3 tools/wav_to_pcm.py ../phone/assets/clips/delete_meetings_thursday.wav \
      board/main/test_clip.pcm
"""
import sys, wave

w = wave.open(sys.argv[1])
assert w.getnchannels() == 1 and w.getframerate() == 16000 and w.getsampwidth() == 2, \
    "the device speaks 16 kHz mono 16 bit only"
data = w.readframes(w.getnframes())
open(sys.argv[2], "wb").write(data)
print(f"{sys.argv[2]}: {len(data)} bytes, {w.getnframes()} samples, "
      f"{w.getnframes()/16000:.1f} s")
