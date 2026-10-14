#include "halo/audio.hpp"

namespace halo {

namespace {

const int8_t kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

const int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

}  // namespace

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len, uint16_t seed) {
    uint16_t crc = seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

uint8_t AdpcmEncoder::encode_one(int16_t sample) {
    int32_t step = kStepTable[index_];
    int32_t diff = static_cast<int32_t>(sample) - predictor_;
    uint8_t nibble = 0;
    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }
    int32_t vpdiff = step >> 3;
    if (diff >= step) {
        nibble |= 4;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        nibble |= 2;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        nibble |= 1;
        vpdiff += step;
    }
    predictor_ = clamp((nibble & 8) ? predictor_ - vpdiff : predictor_ + vpdiff, -32768, 32767);
    index_ = static_cast<int8_t>(clamp(index_ + kIndexTable[nibble], 0, 88));
    return nibble;
}

void AdpcmEncoder::encode(const int16_t* samples, size_t count, uint8_t* out) {
    // Low nibble first, which is standard IMA and what the phone expects.
    for (size_t i = 0; i + 1 < count; i += 2) {
        const uint8_t lo = encode_one(samples[i]);
        const uint8_t hi = encode_one(samples[i + 1]);
        out[i / 2] = static_cast<uint8_t>(lo | (hi << 4));
    }
}

size_t build_chunk(uint8_t* out, uint16_t req, uint8_t rev, uint8_t seg, uint16_t seq,
                   const uint8_t* payload, uint16_t payload_len, AdpcmState at_start,
                   bool final_marker) {
    out[0] = kFormatImaAdpcm16kMono;
    out[1] = final_marker ? 1 : 0;
    put_u16(out + 2, req);
    out[4] = rev;
    out[5] = seg;
    put_u16(out + 6, seq);
    put_u16(out + 8, payload_len);
    // Bytes 10 and 11 are the CRC itself and are not part of what it covers.
    out[12] = static_cast<uint8_t>(at_start.predictor & 0xFF);
    out[13] = static_cast<uint8_t>((static_cast<uint16_t>(at_start.predictor) >> 8) & 0xFF);
    out[14] = at_start.index;
    out[15] = 0;  // reserved
    for (uint16_t i = 0; i < payload_len; ++i) out[kChunkHeaderBytes + i] = payload[i];

    // CRC-16/CCITT-FALSE over header bytes 0 to 9 and the payload, continued
    // across the two pieces so the gap at bytes 10 and 11 is skipped.
    const uint16_t crc = crc16_ccitt_false(out + kChunkHeaderBytes, payload_len,
                                           crc16_ccitt_false(out, 10));
    put_u16(out + 10, crc);
    return kChunkHeaderBytes + payload_len;
}

size_t build_final_marker(uint8_t* out, uint16_t req, uint8_t rev, uint8_t seg, uint16_t seq,
                          uint32_t total_samples) {
    uint8_t payload[kFinalMarkerPayloadBytes];
    put_u16(payload, seq);  // the number of audio chunks that came before it
    put_u32(payload + 2, total_samples);
    return build_chunk(out, req, rev, seg, seq, payload, kFinalMarkerPayloadBytes, AdpcmState{},
                       true);
}

}  // namespace halo
