#include "halo/base64.hpp"

namespace halo {
namespace {

// 0 to 63 for a base64 character, 64 for '=', 255 for anything else. A table
// rather than a chain of comparisons: it is smaller, and it cannot grow a gap
// that quietly accepts a character it should not.
constexpr uint8_t kBad = 255;
constexpr uint8_t kPad = 64;

uint8_t value_of(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c - 'a' + 26);
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return kPad;
    return kBad;
}

}  // namespace

size_t base64_decode(const char* in, size_t len, uint8_t* out, size_t out_size) {
    if (in == nullptr || out == nullptr) return 0;
    if (len == 0 || len % 4 != 0) return 0;
    if (base64_max_bytes(len) > out_size) return 0;

    size_t written = 0;
    for (size_t i = 0; i < len; i += 4) {
        uint8_t v[4];
        for (int j = 0; j < 4; ++j) {
            v[j] = value_of(in[i + j]);
            if (v[j] == kBad) return 0;
        }
        // Padding is only ever the last one or two characters of the whole
        // string, and never the first two of a group.
        if (v[0] == kPad || v[1] == kPad) return 0;
        if (v[2] == kPad && v[3] != kPad) return 0;
        if ((v[2] == kPad || v[3] == kPad) && i + 4 != len) return 0;

        out[written++] = static_cast<uint8_t>((v[0] << 2) | (v[1] >> 4));
        if (v[2] != kPad) out[written++] = static_cast<uint8_t>((v[1] << 4) | (v[2] >> 2));
        if (v[3] != kPad) out[written++] = static_cast<uint8_t>((v[2] << 6) | v[3]);
    }
    return written;
}

}  // namespace halo
