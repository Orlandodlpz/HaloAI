"""IMA ADPCM, 16 kHz mono, low nibble first. Own code instead of the stdlib
audioop module (removed in Python 3.13). Must match phone/lib/ble/ima_adpcm.dart
and the firmware encoder byte for byte; the golden vectors check that."""

INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
]


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


class Encoder:
    """Keeps state across chunks, like the device's continuous stream. Each
    chunk records the state at its start so the phone can decode it alone."""

    def __init__(self):
        self.predictor = 0
        self.index = 0

    def encode(self, samples):
        """Encodes an even number of int16 samples into len/2 bytes."""
        assert len(samples) % 2 == 0
        out = bytearray()
        nibbles = [self._encode_one(s) for s in samples]
        for i in range(0, len(nibbles), 2):
            out.append(nibbles[i] | (nibbles[i + 1] << 4))
        return bytes(out)

    def _encode_one(self, sample):
        step = STEP_TABLE[self.index]
        diff = sample - self.predictor
        nibble = 0
        if diff < 0:
            nibble = 8
            diff = -diff
        vpdiff = step >> 3
        if diff >= step:
            nibble |= 4
            diff -= step
            vpdiff += step
        step >>= 1
        if diff >= step:
            nibble |= 2
            diff -= step
            vpdiff += step
        step >>= 1
        if diff >= step:
            nibble |= 1
            vpdiff += step
        if nibble & 8:
            self.predictor -= vpdiff
        else:
            self.predictor += vpdiff
        self.predictor = _clamp(self.predictor, -32768, 32767)
        self.index = _clamp(self.index + INDEX_TABLE[nibble], 0, 88)
        return nibble


def decode(payload, predictor, index):
    """Reference decoder, same algorithm as the phone's."""
    out = []
    for byte in payload:
        for nibble in (byte & 0x0F, byte >> 4):
            step = STEP_TABLE[index]
            diff = step >> 3
            if nibble & 4:
                diff += step
            if nibble & 2:
                diff += step >> 1
            if nibble & 1:
                diff += step >> 2
            predictor = predictor - diff if nibble & 8 else predictor + diff
            predictor = _clamp(predictor, -32768, 32767)
            index = _clamp(index + INDEX_TABLE[nibble], 0, 88)
            out.append(predictor)
    return out
