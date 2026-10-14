// Just enough JSON for the BLE messages in BRIEFING section 2.
//
// Not a general parser. The messages are small, their shapes are fixed by the
// contract, and a full parser on a device is a liability rather than an asset:
// it is more code, more failure modes, and it would happily accept nonsense that
// the contract forbids. These helpers read named scalars out of a flat object
// and write flat objects, which is all six characteristics ever need.
//
// Tool results and anything else arriving from outside stay data, never
// instructions, so nothing here ever evaluates what it reads.
#pragma once

#include <cstddef>
#include <cstdint>

namespace halo::json {

// Finds "key": <int> in a flat object. Returns false if absent or not a number,
// leaving `out` untouched. Handles negative numbers, which `upto` needs: it is
// -1 before chunk 0 has arrived.
bool get_int(const char* doc, size_t len, const char* key, int32_t* out);

// Finds "key": true|false.
bool get_bool(const char* doc, size_t len, const char* key, bool* out);

// Copies "key": "string" into out, always null terminated. Returns false if
// absent; truncates rather than overflowing.
bool get_str(const char* doc, size_t len, const char* key, char* out, size_t out_size);

// Builds a flat object into a fixed buffer. Every append is bounds checked and
// sets a sticky overflow flag, so a message that does not fit is reported rather
// than silently truncated into invalid JSON.
class Writer {
public:
    Writer(char* buf, size_t size) : buf_(buf), size_(size) { reset(); }

    void reset();
    void key_str(const char* key, const char* value);
    void key_int(const char* key, int32_t value);
    void key_bool(const char* key, bool value);
    void key_null(const char* key);

    // Closes the object. Returns nullptr if anything overflowed.
    const char* finish();
    size_t length() const { return len_; }
    bool overflowed() const { return overflow_; }

private:
    void put(char c);
    void put_str(const char* s);
    void put_escaped(const char* s);
    void comma();

    char* buf_;
    size_t size_;
    size_t len_ = 0;
    bool first_ = true;
    bool overflow_ = false;
    bool done_ = false;
};

}  // namespace halo::json
