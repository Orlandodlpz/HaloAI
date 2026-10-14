// The device's BLE service: one custom service, six characteristics, exactly as
// agreed with Carlos on 2026-10-13 (BRIEFING section 2, and the detail in
// phone/docs/ble_fields_draft.md).
//
// The device is the peripheral and the phone is the central. The device never
// talks to anything else: no Wi-Fi, no cloud, ever. That is a hard constraint,
// not a default.
#pragma once

#include <cstddef>
#include <cstdint>

namespace halo::board::ble {

// What arrives from the phone. The JSON is NOT null terminated; use the length.
struct Handlers {
    void (*on_audio_ack)(const char* json, size_t len) = nullptr;
    void (*on_screen)(const char* json, size_t len) = nullptr;
    void (*on_settings)(const char* json, size_t len) = nullptr;
    void (*on_wallpaper)(const uint8_t* data, size_t len) = nullptr;
    // Fires when the phone turns on Events notifications, which is the moment
    // `hello` must be sent: anything sent before that is dropped by the stack
    // because nobody is subscribed yet.
    void (*on_events_subscribed)() = nullptr;
    void (*on_disconnect)() = nullptr;
};

bool start(const char* device_name, const Handlers& handlers);

bool connected();
// Negotiated ATT MTU. A 176 byte audio chunk needs at least 179.
uint16_t mtu();

// Raw bytes on the Audio characteristic. Returns false if the notify failed,
// which is how the caller learns the link is stalling.
bool notify_audio(const uint8_t* data, size_t len);

// JSON on the Events characteristic, fragmented with the agreed 2 byte prefix.
bool notify_event(const char* json, size_t len);

}  // namespace halo::board::ble
