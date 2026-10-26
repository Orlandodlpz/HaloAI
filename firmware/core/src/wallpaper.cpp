#include "halo/wallpaper.hpp"

#include <cstring>

#include "halo/audio.hpp"   // crc16_ccitt_false: the same CRC the audio chunks use
#include "halo/base64.hpp"
#include "halo/json.hpp"

namespace halo::wp {
namespace {

// One receiver, on one task, so these are shared rather than per instance: a
// chunk is decoded and written before the next one is looked at. On the device
// they must not be on the stack, which is 8 KB and already holds a display list.
char g_b64[1025];
uint8_t g_bytes[kMaxChunkBytes];

}  // namespace

bool size_is_supported(uint16_t w, uint16_t h) {
    return (w == kLandscapeW && h == kLandscapeH) || (w == kPortraitW && h == kPortraitH);
}

void Receiver::reset() {
    if (active_) sink_.abort();
    active_ = false;
    awaiting_resend_ = false;
    received_ = 0;
    crc_ = 0xFFFF;
}

Answer Receiver::nothing(const char* note) {
    Answer a;
    a.note = note;
    return a;
}

Answer Receiver::refuse(uint8_t wp, const char* note) {
    reset();
    Answer a;
    a.send = true;
    a.wp = wp;
    a.ok = false;
    a.note = note;
    return a;
}

Answer Receiver::ask_resend(uint8_t wp, uint32_t from, const char* note) {
    awaiting_resend_ = true;
    Answer a;
    a.send = true;
    a.wp = wp;
    a.ok = false;
    a.resend = true;
    a.resend_from = from;
    a.note = note;
    return a;
}

Answer Receiver::feed(const char* doc, size_t len) {
    int32_t wp = 0;
    // Without a transfer ID there is nobody to answer: the phone matches answers
    // by `wp` and would ignore anything else. Drop it and say so in the log.
    if (!json::get_int(doc, len, "wp", &wp) || wp < 0 || wp > 255)
        return nothing("wallpaper write with no usable wp");
    const uint8_t id = static_cast<uint8_t>(wp);

    int32_t w = 0, h = 0, off = 0;
    if (!json::get_int(doc, len, "w", &w) || !json::get_int(doc, len, "h", &h) ||
        !json::get_int(doc, len, "off", &off))
        return refuse(id, "wallpaper chunk missing w, h or off");
    if (w <= 0 || h <= 0 || !size_is_supported(static_cast<uint16_t>(w), static_cast<uint16_t>(h)))
        return refuse(id, "wallpaper is not a size this device can show");
    if (off < 0 || static_cast<uint32_t>(off) >= kImageBytes)
        return refuse(id, "wallpaper offset is outside the image");

    if (!json::get_str(doc, len, "data", g_b64, sizeof(g_b64)))
        return refuse(id, "wallpaper chunk has no data");
    const size_t n = base64_decode(g_b64, std::strlen(g_b64), g_bytes, sizeof(g_bytes));
    if (n == 0) return ask_resend(id, received_, "wallpaper chunk did not decode");

    const uint32_t at = static_cast<uint32_t>(off);
    const bool restart = (at == 0);
    const bool mine = active_ && id == wp_;

    if (!mine || restart) {
        // A new transfer ID abandons whatever was in progress, and so does an
        // offset of 0, which is how the phone restarts after a CRC failure.
        if (!mine && !restart)
            return ask_resend(id, 0, "chunk for a transfer this device never started");
        if (active_) sink_.abort();
        active_ = false;
        if (!sink_.begin(static_cast<uint16_t>(w), static_cast<uint16_t>(h)))
            return refuse(id, "wallpaper storage would not open");
        active_ = true;
        awaiting_resend_ = false;
        wp_ = id;
        w_ = static_cast<uint16_t>(w);
        h_ = static_cast<uint16_t>(h);
        received_ = 0;
        crc_ = 0xFFFF;
    } else {
        if (w != w_ || h != h_) return refuse(id, "wallpaper changed size mid transfer");
        if (at != received_) {
            // One lost chunk makes every later one land at the wrong offset.
            // Asking once and then staying quiet is the difference between one
            // answer and one per remaining chunk.
            if (awaiting_resend_) return nothing("chunk out of order, resend already asked for");
            return ask_resend(id, received_, "wallpaper chunk arrived out of order");
        }
        awaiting_resend_ = false;
    }

    if (received_ + n > kImageBytes) return refuse(id, "wallpaper is longer than the image");
    if (!sink_.write(received_, g_bytes, n)) return refuse(id, "wallpaper storage write failed");
    crc_ = crc16_ccitt_false(g_bytes, n, crc_);
    received_ += static_cast<uint32_t>(n);

    // The CRC field, and only the CRC field, marks the end of the image.
    int32_t want = 0;
    if (!json::get_int(doc, len, "crc", &want)) return nothing("");

    if (received_ != kImageBytes)
        return ask_resend(id, received_, "wallpaper ended short of the whole image");
    if (want < 0 || static_cast<uint16_t>(want) != crc_)
        return ask_resend(id, 0, "wallpaper CRC did not match: starting over");
    if (!sink_.commit(w_, h_, crc_)) return refuse(id, "wallpaper could not be stored");

    active_ = false;
    awaiting_resend_ = false;
    Answer a;
    a.send = true;
    a.wp = id;
    a.ok = true;
    a.note = "wallpaper stored";
    return a;
}

}  // namespace halo::wp
