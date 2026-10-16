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

// Arrays of strings: screen_lines and candidates, the only two in the contract.
//
// Deliberately an index based reader rather than anything that hands back a
// pointer into the document. The document is the BLE receive buffer, which the
// next write overwrites, and a screen has to survive long enough to be redrawn
// when the knob scrolls it.

// Number of elements in the array at "key". 0 if the key is absent, is not an
// array, is empty, or is never closed.
size_t array_len(const char* doc, size_t len, const char* key);

// Copies element `index` of the array at "key" into out, always null terminated.
// False if the key is absent, is not an array, has no such element, or that
// element is not a string. Truncates rather than overflowing, like get_str.
bool get_str_at(const char* doc, size_t len, const char* key, size_t index, char* out,
                size_t out_size);

// Builds a flat object into a fixed buffer. Every append is bounds checked and
// sets a sticky overflow flag, so a message that does not fit is reported rather
// than silently truncated into invalid JSON.
class Writer {
public:
    Writer(char* buf, size_t size) : buf_(buf), size_(size) { reset(); }

    void reset();
    void key_str(const char* key, const char* value);
    void key_int(const char* key, int32_t value);
    // u32 values the contract has that do not fit in an int32: `boot` is random
    // over the whole range, and `ms` is a duration.
    void key_uint(const char* key, uint32_t value);
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
