#include "wallpaper_store.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

namespace halo::board {
namespace {

constexpr const char* kTag = "halo.wp";

const esp_partition_t* as_part(const void* p) {
    return static_cast<const esp_partition_t*>(p);
}

}  // namespace

bool WallpaperStore::init() {
    // Declared `spiffs` in partitions.csv because the table needs a data subtype;
    // there is no filesystem in it, only our two slots.
    part_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                     "wallpaper");
    if (part_ == nullptr) {
        ESP_LOGW(kTag, "no wallpaper partition: the phone will be told this device cannot store one");
        return false;
    }
    if (as_part(part_)->size < 2 * kSlotBytes) {
        ESP_LOGE(kTag, "wallpaper partition is %u bytes, two slots need %u",
                 static_cast<unsigned>(as_part(part_)->size),
                 static_cast<unsigned>(2 * kSlotBytes));
        part_ = nullptr;
        return false;
    }

    // Whichever slot holds the newer complete image wins. A slot half written
    // when the power went out has no record, so it loses by construction.
    Record best{};
    uint8_t best_slot = 0xFF;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        Record r{};
        if (!read_record(slot, &r)) continue;
        if (best_slot == 0xFF || r.seq > best.seq) {
            best = r;
            best_slot = slot;
        }
    }
    if (best_slot == 0xFF) {
        ESP_LOGI(kTag, "no wallpaper stored yet");
        return true;
    }
    seq_ = best.seq;
    if (!map_slot(best_slot, best.w, best.h)) return true;
    ESP_LOGI(kTag, "wallpaper %ux%u from slot %u, crc %04x", best.w, best.h, best_slot, best.crc);
    return true;
}

bool WallpaperStore::read_record(uint8_t slot, Record* out) const {
    if (part_ == nullptr) return false;
    Record r{};
    if (esp_partition_read(as_part(part_), slot * kSlotBytes + kRecordOffset, &r, sizeof(r)) != ESP_OK)
        return false;
    if (r.magic != kMagic) return false;
    if (!wp::size_is_supported(r.w, r.h)) return false;
    *out = r;
    return true;
}

bool WallpaperStore::map_slot(uint8_t slot, uint16_t w, uint16_t h) {
    if (map_handle_ != 0) {
        esp_partition_munmap(static_cast<esp_partition_mmap_handle_t>(map_handle_));
        map_handle_ = 0;
        image_ = nullptr;
    }
    const void* ptr = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    // Mapped rather than read into RAM: 48000 bytes of internal memory is a lot
    // to spend on a picture, and the panel reads it once per refresh.
    const esp_err_t err = esp_partition_mmap(as_part(part_), slot * kSlotBytes, wp::kImageBytes,
                                             ESP_PARTITION_MMAP_DATA, &ptr, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "could not map the wallpaper: %s", esp_err_to_name(err));
        return false;
    }
    map_handle_ = static_cast<uint32_t>(handle);
    image_ = static_cast<const uint8_t*>(ptr);
    current_slot_ = slot;
    icon_.bits = image_;
    icon_.w = w;
    icon_.h = h;
    // The phone's bytes mean 1 = white; the display list means 1 = ink.
    icon_.invert = true;
    return true;
}

const Icon* WallpaperStore::current(bool portrait) const {
    if (image_ == nullptr) return nullptr;
    const bool stored_portrait = icon_.h > icon_.w;
    if (stored_portrait != portrait) return nullptr;
    return &icon_;
}

bool WallpaperStore::begin(uint16_t w, uint16_t h) {
    if (part_ == nullptr) return false;
    if (!wp::size_is_supported(w, h)) return false;
    if (staging_ == nullptr) {
        // PSRAM by preference: 48 KB of internal memory is a lot to hold while
        // the radio is busy. Either is fine, and a board with neither says no
        // here rather than halfway through.
        staging_ = static_cast<uint8_t*>(heap_caps_malloc(wp::kImageBytes, MALLOC_CAP_SPIRAM));
        if (staging_ == nullptr)
            staging_ = static_cast<uint8_t*>(heap_caps_malloc(wp::kImageBytes, MALLOC_CAP_8BIT));
        if (staging_ == nullptr) {
            ESP_LOGE(kTag, "no room to receive a wallpaper: %u bytes free",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
            return false;
        }
    }
    staged_ = 0;
    // Never the slot being shown: the picture on the glass has to survive a
    // transfer that fails halfway, and so does the one a reboot would restore.
    writing_slot_ = (current_slot_ == 0) ? 1 : 0;
    return true;
}

bool WallpaperStore::write(uint32_t off, const uint8_t* data, size_t len) {
    if (staging_ == nullptr || writing_slot_ == 0xFF) return false;
    // The receiver only ever hands over the next bytes in order, and a mismatch
    // here would mean the two disagree about what has been stored.
    if (off != staged_) return false;
    if (off + len > wp::kImageBytes) return false;
    std::memcpy(staging_ + off, data, len);
    staged_ += static_cast<uint32_t>(len);
    return true;
}

bool WallpaperStore::commit(uint16_t w, uint16_t h, uint16_t crc) {
    if (part_ == nullptr || staging_ == nullptr || writing_slot_ == 0xFF) return false;
    if (staged_ != wp::kImageBytes) {
        ESP_LOGE(kTag, "commit with %u of %u bytes", static_cast<unsigned>(staged_),
                 static_cast<unsigned>(wp::kImageBytes));
        return false;
    }

    // Every flash operation happens here, with nothing in flight: the phone has
    // sent its last chunk and is waiting for this answer.
    const uint32_t base = writing_slot_ * kSlotBytes;
    const uint32_t span = kRecordOffset + 4096;
    esp_err_t err = esp_partition_erase_range(as_part(part_), base, span);
    if (err == ESP_OK) err = esp_partition_write(as_part(part_), base, staging_, wp::kImageBytes);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "wallpaper flash write failed: %s", esp_err_to_name(err));
        return false;
    }

    Record r{};
    r.magic = kMagic;
    r.seq = seq_ + 1;
    r.w = w;
    r.h = h;
    r.crc = crc;
    // The record is written last and nothing else marks a slot complete, so a
    // power cut anywhere above this line leaves the old wallpaper in charge.
    if (esp_partition_write(as_part(part_), base + kRecordOffset, &r, sizeof(r)) != ESP_OK)
        return false;

    const uint8_t slot = writing_slot_;
    abort();   // the staging buffer has done its job
    seq_ = r.seq;
    if (!map_slot(slot, w, h)) return false;
    ESP_LOGI(kTag, "wallpaper %ux%u stored in slot %u, crc %04x", w, h, slot, crc);
    return true;
}

void WallpaperStore::abort() {
    // Nothing on the glass to undo: the slot being written is not the one being
    // shown, and it is not readable until its record is written. The 48 KB goes
    // back, because an abandoned transfer must not cost memory until the next one.
    if (staging_ != nullptr) {
        heap_caps_free(staging_);
        staging_ = nullptr;
    }
    staged_ = 0;
    writing_slot_ = 0xFF;
}

}  // namespace halo::board
