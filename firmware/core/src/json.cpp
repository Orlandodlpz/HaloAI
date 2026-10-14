#include "halo/json.hpp"

namespace halo::json {

namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

size_t key_len(const char* key) {
    size_t n = 0;
    while (key[n]) ++n;
    return n;
}

// Finds the position just past "key": , skipping whitespace. Only matches a key
// at the top level of a flat object, which is all the contract uses.
const char* find_value(const char* doc, size_t len, const char* key) {
    const size_t klen = key_len(key);
    if (klen == 0 || len < klen + 3) return nullptr;
    for (size_t i = 0; i + klen + 2 < len; ++i) {
        if (doc[i] != '"') continue;
        bool match = true;
        for (size_t k = 0; k < klen; ++k) {
            if (doc[i + 1 + k] != key[k]) {
                match = false;
                break;
            }
        }
        if (!match || doc[i + 1 + klen] != '"') continue;
        size_t j = i + klen + 2;
        while (j < len && is_ws(doc[j])) ++j;
        if (j >= len || doc[j] != ':') continue;
        ++j;
        while (j < len && is_ws(doc[j])) ++j;
        return j < len ? doc + j : nullptr;
    }
    return nullptr;
}

}  // namespace

bool get_int(const char* doc, size_t len, const char* key, int32_t* out) {
    const char* p = find_value(doc, len, key);
    if (p == nullptr) return false;
    const char* end = doc + len;
    bool negative = false;
    if (p < end && (*p == '-' || *p == '+')) {
        negative = (*p == '-');
        ++p;
    }
    if (p >= end || *p < '0' || *p > '9') return false;
    int64_t value = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        if (value > 2147483647LL) return false;  // refuse rather than wrap
        ++p;
    }
    *out = static_cast<int32_t>(negative ? -value : value);
    return true;
}

bool get_bool(const char* doc, size_t len, const char* key, bool* out) {
    const char* p = find_value(doc, len, key);
    if (p == nullptr) return false;
    const size_t left = static_cast<size_t>(doc + len - p);
    if (left >= 4 && p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') {
        *out = true;
        return true;
    }
    if (left >= 5 && p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') {
        *out = false;
        return true;
    }
    return false;
}

bool get_str(const char* doc, size_t len, const char* key, char* out, size_t out_size) {
    if (out_size == 0) return false;
    const char* p = find_value(doc, len, key);
    if (p == nullptr || *p != '"') return false;
    ++p;
    const char* end = doc + len;
    size_t n = 0;
    while (p < end && *p != '"') {
        char c = *p;
        if (c == '\\' && p + 1 < end) {
            ++p;
            switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default: c = *p; break;  // quote and backslash escape to themselves
            }
        }
        if (n + 1 < out_size) out[n++] = c;
        ++p;
    }
    out[n] = '\0';
    return p < end;  // false if the string was never closed
}

// ---------------------------------------------------------------- writer

void Writer::reset() {
    len_ = 0;
    first_ = true;
    overflow_ = false;
    done_ = false;
    put('{');
}

void Writer::put(char c) {
    if (len_ + 1 >= size_) {  // keep room for the terminator
        overflow_ = true;
        return;
    }
    buf_[len_++] = c;
}

void Writer::put_str(const char* s) {
    for (; *s; ++s) put(*s);
}

void Writer::put_escaped(const char* s) {
    put('"');
    for (; *s; ++s) {
        const char c = *s;
        if (c == '"' || c == '\\') {
            put('\\');
            put(c);
        } else if (c == '\n') {
            put('\\');
            put('n');
        } else if (static_cast<unsigned char>(c) < 0x20) {
            put(' ');  // control characters never belong in these messages
        } else {
            put(c);
        }
    }
    put('"');
}

void Writer::comma() {
    if (!first_) put(',');
    first_ = false;
}

void Writer::key_str(const char* key, const char* value) {
    comma();
    put_escaped(key);
    put(':');
    put_escaped(value);
}

void Writer::key_int(const char* key, int32_t value) {
    comma();
    put_escaped(key);
    put(':');
    if (value < 0) {
        put('-');
    }
    uint32_t v = value < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(value))
                           : static_cast<uint32_t>(value);
    char digits[12];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n-- > 0) put(digits[n]);
}

void Writer::key_bool(const char* key, bool value) {
    comma();
    put_escaped(key);
    put(':');
    put_str(value ? "true" : "false");
}

void Writer::key_null(const char* key) {
    comma();
    put_escaped(key);
    put(':');
    put_str("null");
}

const char* Writer::finish() {
    if (!done_) {
        put('}');
        done_ = true;
    }
    if (overflow_) return nullptr;
    buf_[len_] = '\0';
    return buf_;
}

}  // namespace halo::json
