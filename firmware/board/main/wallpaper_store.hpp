// The wallpaper in flash. The board half of halo/wallpaper.hpp: it owns the
// partition and nothing else, and every decision about what to accept was already
// made above the display boundary, where it can be tested on a laptop.
//
// Two slots, so the picture on the glass survives a transfer that fails halfway.
// A slot is only ever read after its record says the whole image is there and the
// CRC matched, and the record is the last thing written.
//
// The image is assembled in RAM and written to flash once, after the last chunk,
// while the phone is idle waiting for its answer. Writing it as it arrived also
// worked, but it couples a flash erase (tens of ms with the cache suspended, so
// the radio task cannot run) to a stream of incoming writes, for the sake of
// 44 KB. Staging costs 48 KB for the length of a transfer, taken from PSRAM when
// there is any, and nothing at rest.
//
// For the record, because the first version of this comment said otherwise: the
// dropped chunks measured on 2026-10-25 were **not** caused by flash. Staging in
// RAM did not change them. They are the Mac's: CoreBluetooth discards writes
// without response when its own queue fills and tells nobody. The board accepted
// 109 kbit/s of image with zero loss and kept accepting at 400+ kbit/s with gaps
// in it, never once overflowing its own inbox.

#pragma once

#include <cstdint>

#include "halo/layout.hpp"
#include "halo/wallpaper.hpp"

namespace halo::board {

class WallpaperStore : public wp::Sink {
public:
    // Finds the partition and maps the newest complete image. False means this
    // firmware cannot store a wallpaper at all, which is what `hello` reports.
    bool init();
    bool ready() const { return part_ != nullptr; }

    // The stored image, or null when there is none or it was made for the other
    // orientation. The phone sends a wallpaper in the orientation that was set
    // when the user picked it; if that setting changes later, the device shows
    // its name again rather than a sideways photo, and the phone can send a new
    // one from the original photo.
    const Icon* current(bool portrait) const;

    // wp::Sink. Driven by halo::wp::Receiver on the main task only.
    bool begin(uint16_t w, uint16_t h) override;
    bool write(uint32_t off, const uint8_t* data, size_t len) override;
    bool commit(uint16_t w, uint16_t h, uint16_t crc) override;
    void abort() override;

private:
    // 64 KB a slot: the 48000 byte image, then its record in the next sector.
    // Both are block aligned, so erasing a slot never touches the other one.
    static constexpr uint32_t kSlotBytes = 0x10000;
    static constexpr uint32_t kRecordOffset = 0xC000;
    static constexpr uint32_t kMagic = 0x31505748;  // "HWP1"

    struct Record {
        uint32_t magic;
        uint32_t seq;   // the higher of the two valid slots is the current one
        uint16_t w, h;
        uint16_t crc;
        uint16_t reserved;
    };

    bool read_record(uint8_t slot, Record* out) const;
    bool map_slot(uint8_t slot, uint16_t w, uint16_t h);

    const void* part_ = nullptr;        // esp_partition_t, kept opaque to the header
    const uint8_t* image_ = nullptr;    // memory mapped, so the image costs no RAM
    uint32_t map_handle_ = 0;
    Icon icon_{};
    uint8_t current_slot_ = 0xFF;
    uint32_t seq_ = 0;

    // The transfer in progress: the slot it will land in, and the image being
    // assembled. PSRAM when there is some, internal memory otherwise, and held
    // only while an image is arriving.
    uint8_t writing_slot_ = 0xFF;
    uint8_t* staging_ = nullptr;
    uint32_t staged_ = 0;
};

}  // namespace halo::board
