// Receiving the wallpaper the user picked on the phone (P4 on the phone, F1 here).
//
// Contract: BRIEFING section 2 says "JSON chunks with offset and checksum, image
// data base64", and the field names are Carlos's P4 proposal in
// phone/docs/ble_fields_draft.md, answered in firmware/docs/ble_fields.md:
//
//   {"wp": 7, "w": 800, "h": 480, "off": 0, "data": "<base64>"}   and "crc" on the last
//
// The phone has already cropped, resized and dithered the photo, so what arrives
// is exactly the bits a panel shows: 1 bit per pixel, rows top to bottom, most
// significant bit leftmost, **1 means white**. That is the phone's and both
// panels' framebuffer convention, and it is the opposite of the display list's
// BitmapCmd, where a set bit is ink. The image is stored as it arrived and
// inverted at draw time by the `invert` flag, so the bytes in flash, on air and
// in the phone's preview are the same bytes and can be compared.
//
// This half is in core/ because it decides what ends up on the glass and what the
// phone is told, and because the whole of it can be tested on a laptop. The board
// glue below it owns flash and nothing else.
#pragma once

#include <cstddef>
#include <cstdint>

namespace halo::wp {

// Layout draws on the badge's canvas, so a wallpaper is always the whole canvas:
// 800 x 480 landscape or 480 x 800 portrait, 48000 bytes either way. A panel that
// shows less of it (the 272 row development board) crops or scales in its own
// backend, which is where every other panel difference lives.
constexpr uint16_t kLandscapeW = 800, kLandscapeH = 480;
constexpr uint16_t kPortraitW = 480, kPortraitH = 800;
constexpr uint32_t kImageBytes = 48000;

bool size_is_supported(uint16_t w, uint16_t h);

// The most image bytes one chunk may carry. At the negotiated MTU of 256 a chunk
// is 141 bytes; this leaves room for a phone that gets a larger MTU and sends
// bigger chunks, and refuses anything past it rather than overrunning.
constexpr size_t kMaxChunkBytes = 768;

// Where the bytes go. The device writes them to the `wallpaper` flash partition;
// the host tests write them to an array. Every call returns false on failure, and
// a failed write ends the transfer rather than leaving a half image behind.
class Sink {
public:
    virtual ~Sink() = default;

    // A new image is starting. Anything kept from a previous attempt is dropped.
    // The wallpaper already on the glass must survive until commit().
    virtual bool begin(uint16_t w, uint16_t h) = 0;
    virtual bool write(uint32_t off, const uint8_t* data, size_t len) = 0;
    // Every byte arrived and the CRC matched. Only now may the new image replace
    // the old one.
    virtual bool commit(uint16_t w, uint16_t h, uint16_t crc) = 0;
    virtual void abort() = 0;
};

// What to send back on Events. Mid transfer chunks are not answered at all: the
// phone waits for the end, and 341 acknowledgments would drown the link.
struct Answer {
    bool send = false;
    uint8_t wp = 0;
    bool ok = false;
    bool resend = false;      // resend_from is present
    uint32_t resend_from = 0;
    const char* note = "";    // for our log only; never sent to the phone
};

class Receiver {
public:
    explicit Receiver(Sink& sink) : sink_(sink) {}

    // One whole reassembled Wallpaper write.
    Answer feed(const char* doc, size_t len);

    // A transfer belongs to one connection: the phone's transfer IDs start over
    // on a reconnect, and a half written image must never be completed by them.
    void reset();

    bool busy() const { return active_; }
    uint32_t received() const { return received_; }

private:
    Answer nothing(const char* note);
    Answer refuse(uint8_t wp, const char* note);
    Answer ask_resend(uint8_t wp, uint32_t from, const char* note);

    Sink& sink_;
    bool active_ = false;
    bool awaiting_resend_ = false;  // suppresses an answer per lost chunk
    uint8_t wp_ = 0;
    uint16_t w_ = 0, h_ = 0;
    uint32_t received_ = 0;
    uint16_t crc_ = 0xFFFF;
};

}  // namespace halo::wp
