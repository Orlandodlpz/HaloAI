// Base64 decoding, for the one thing in the contract that carries binary inside
// JSON: the wallpaper image (BRIEFING section 2, "image data base64").
//
// Decode only. Nothing on the device ever needs to encode: everything the device
// sends the phone is small enough to say in plain JSON.
//
// Like the JSON reader next to it, this trusts nothing it is handed. The input is
// a BLE write from the phone, the output buffer is fixed, and a string that is
// not valid base64 is refused rather than half decoded.
#pragma once

#include <cstddef>
#include <cstdint>

namespace halo {

// Decodes `len` characters at `in` into `out`. Returns the number of bytes
// written, or 0 on any problem: an invalid character, bad padding, a length that
// is not a multiple of 4, or a result larger than `out_size`.
//
// 0 is also the honest answer for an empty string, which is why callers treat an
// empty `data` field as a protocol error rather than an empty chunk.
size_t base64_decode(const char* in, size_t len, uint8_t* out, size_t out_size);

// How many bytes `len` base64 characters decode to at most, for sizing buffers.
constexpr size_t base64_max_bytes(size_t len) { return len / 4 * 3; }

}  // namespace halo
