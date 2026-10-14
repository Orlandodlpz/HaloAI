// IMA ADPCM encoding and the audio chunk layout from BRIEFING section 2.
//
// This has to agree byte for byte with two other implementations that were
// written independently: `phone/lib/ble/ima_adpcm.dart` and the Python reference
// in `phone/tools/audio_reference/`. Carlos shipped golden vectors for exactly
// that purpose, and `test_audio.cpp` checks every chunk against them.
//
// Hardware independent on purpose: no microphone, no radio, no board headers.
// F2 feeds it real samples from the ES8311 and pushes the chunks over BLE.
#pragma once

#include <cstddef>
#include <cstdint>

namespace halo {

// 20 ms at 16 kHz mono. Two samples per byte, so 160 payload bytes per chunk.
constexpr size_t kSamplesPerChunk = 320;
constexpr size_t kChunkHeaderBytes = 16;
constexpr size_t kChunkPayloadBytes = kSamplesPerChunk / 2;
constexpr size_t kChunkBytes = kChunkHeaderBytes + kChunkPayloadBytes;  // 176

// Codec ID in header byte 0. Anything else is rejected by the phone.
constexpr uint8_t kFormatImaAdpcm16kMono = 1;

// The final marker's payload: chunk count then total samples.
constexpr size_t kFinalMarkerPayloadBytes = 6;

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len, uint16_t seed = 0xFFFF);

// Decoder state at the start of a chunk. Every chunk carries its own copy, so a
// lost chunk never corrupts the ones after it.
struct AdpcmState {
    int16_t predictor = 0;
    uint8_t index = 0;
};

// Keeps state across chunks, the way a continuous hold does.
class AdpcmEncoder {
public:
    void reset() {
        predictor_ = 0;
        index_ = 0;
    }
    AdpcmState state() const {
        return AdpcmState{static_cast<int16_t>(predictor_), static_cast<uint8_t>(index_)};
    }
    // Resume from a state recorded earlier. Needed to resend a chunk without
    // replaying the whole segment: each chunk already carries the state at its
    // own start, so re-encoding from it reproduces identical bytes.
    void set_state(AdpcmState s) {
        predictor_ = s.predictor;
        index_ = static_cast<int8_t>(s.index);
    }

    // Encodes an even number of samples into count/2 bytes, low nibble first.
    void encode(const int16_t* samples, size_t count, uint8_t* out);

private:
    uint8_t encode_one(int16_t sample);

    int32_t predictor_ = 0;
    int8_t index_ = 0;
};

// Writes one audio chunk. `out` needs kChunkHeaderBytes + payload_len bytes.
// Returns the number of bytes written.
size_t build_chunk(uint8_t* out, uint16_t req, uint8_t rev, uint8_t seg, uint16_t seq,
                   const uint8_t* payload, uint16_t payload_len, AdpcmState at_start,
                   bool final_marker = false);

// The final marker closes the segment. FINALIZE AUDIO waits for its acknowledgment;
// `record_end` is only informational.
size_t build_final_marker(uint8_t* out, uint16_t req, uint8_t rev, uint8_t seg, uint16_t seq,
                          uint32_t total_samples);

}  // namespace halo
