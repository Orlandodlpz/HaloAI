// The firmware's ADPCM encoder and chunk builder against Carlos's golden vectors.
//
// This is the one test that proves two independently written implementations
// agree. If it passes, audio recorded on the device decodes on the phone. If it
// fails, the Bluetooth link is broken in a way that would only show up as
// garbled transcripts on real hardware, days later.
#include "golden_adpcm.hpp"
#include "halo/audio.hpp"
#include "harness.hpp"

using namespace halo;
using namespace halo::test;

namespace {

uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    return static_cast<uint8_t>(c - 'A' + 10);
}

// Returns the number of bytes decoded from a hex string.
size_t from_hex(const char* hex, uint8_t* out, size_t max) {
    size_t n = 0;
    for (const char* p = hex; p[0] && p[1] && n < max; p += 2)
        out[n++] = static_cast<uint8_t>((hex_nibble(p[0]) << 4) | hex_nibble(p[1]));
    return n;
}

}  // namespace

HALO_TEST(audio_chunks_match_the_phones_golden_vectors_byte_for_byte) {
    AdpcmEncoder enc;
    uint8_t mine[kChunkBytes];
    uint8_t theirs[kChunkBytes];

    const int audio_chunks = golden::kChunkCount - 1;  // the last one is the final marker
    int sample = 0;
    for (int seq = 0; seq < audio_chunks; ++seq) {
        // The last chunk is zero padded to an even sample count; the final
        // marker says where the real audio ends.
        int16_t part[kSamplesPerChunk] = {};
        size_t count = 0;
        while (count < kSamplesPerChunk && sample < golden::kPcmCount)
            part[count++] = golden::kPcm[sample++];
        if (count % 2) part[count++] = 0;

        const AdpcmState at_start = enc.state();
        uint8_t payload[kChunkPayloadBytes];
        enc.encode(part, count, payload);

        const size_t n = build_chunk(mine, golden::kReq, golden::kRev, golden::kSeg,
                                     static_cast<uint16_t>(seq), payload,
                                     static_cast<uint16_t>(count / 2), at_start);
        const size_t want = from_hex(golden::kChunksHex[seq], theirs, sizeof(theirs));
        CHECK(n == want);
        for (size_t i = 0; i < n && i < want; ++i) {
            if (mine[i] != theirs[i]) {
                ::halo::test::fail(__FILE__, __LINE__,
                                   "chunk " + std::to_string(seq) + " byte " + std::to_string(i) +
                                       ": got " + std::to_string(mine[i]) + ", phone expects " +
                                       std::to_string(theirs[i]));
                return;
            }
        }
    }

    // The final marker: same header, flag bit 0 set, chunk count then total samples.
    const size_t n = build_final_marker(mine, golden::kReq, golden::kRev, golden::kSeg,
                                        static_cast<uint16_t>(audio_chunks),
                                        golden::kTotalSamples);
    const size_t want = from_hex(golden::kChunksHex[audio_chunks], theirs, sizeof(theirs));
    CHECK(n == want);
    for (size_t i = 0; i < n && i < want; ++i) {
        if (mine[i] != theirs[i]) {
            ::halo::test::fail(__FILE__, __LINE__,
                               "final marker byte " + std::to_string(i) + ": got " +
                                   std::to_string(mine[i]) + ", phone expects " +
                                   std::to_string(theirs[i]));
            return;
        }
    }
}

HALO_TEST(audio_chunk_is_the_176_bytes_the_contract_promises) {
    CHECK(kChunkHeaderBytes == 16);
    CHECK(kChunkBytes == 176);
    CHECK(kSamplesPerChunk == 320);
}

HALO_TEST(audio_every_chunk_carries_the_decoder_state_at_its_own_start) {
    // This is what makes a lost chunk harmless: the next one decodes on its own.
    AdpcmEncoder enc;
    int16_t part[kSamplesPerChunk];
    for (size_t i = 0; i < kSamplesPerChunk; ++i)
        part[i] = static_cast<int16_t>((i * 517) % 20000 - 10000);

    uint8_t payload[kChunkPayloadBytes];
    uint8_t chunk[kChunkBytes];

    const AdpcmState first = enc.state();
    enc.encode(part, kSamplesPerChunk, payload);
    const AdpcmState second = enc.state();
    CHECK(first.predictor != second.predictor || first.index != second.index);

    build_chunk(chunk, 1, 0, 0, 1, payload, kChunkPayloadBytes, second);
    const int16_t stored = static_cast<int16_t>(chunk[12] | (chunk[13] << 8));
    CHECK(stored == second.predictor);
    CHECK(chunk[14] == second.index);
}

HALO_TEST(audio_crc_covers_the_header_and_payload_but_not_itself) {
    uint8_t chunk[kChunkBytes];
    uint8_t payload[kChunkPayloadBytes] = {};
    build_chunk(chunk, 7, 1, 2, 3, payload, kChunkPayloadBytes, AdpcmState{});
    const uint16_t stored = static_cast<uint16_t>(chunk[10] | (chunk[11] << 8));
    const uint16_t recomputed =
        crc16_ccitt_false(chunk + kChunkHeaderBytes, kChunkPayloadBytes,
                          crc16_ccitt_false(chunk, 10));
    CHECK(stored == recomputed);

    // A flipped payload byte must change it, or the check is worthless.
    chunk[kChunkHeaderBytes + 5] ^= 0x01;
    const uint16_t after = crc16_ccitt_false(chunk + kChunkHeaderBytes, kChunkPayloadBytes,
                                             crc16_ccitt_false(chunk, 10));
    CHECK(after != stored);
}
