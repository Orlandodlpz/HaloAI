// What the device says on the screens it owns.
//
// Twelve of the 21 states get their words from the backend: it owns the request,
// so it owns the wording, and the device draws what arrives. The other nine are
// the device's own (BRIEFING section 2, "State ownership"), and nothing sends
// text for them. This is that text.
//
// It lives in core/ for the same reason the state machine does: the words a user
// reads before confirming something irreversible are worth testing, and here they
// can be checked against the published text budget without a panel.
#pragma once

#include "halo/layout.hpp"
#include "halo/states.hpp"

namespace halo {

// What the device knows about itself. None of it comes from the backend.
struct DeviceStatus {
    uint8_t battery_pct = 100;
    bool phone_linked = false;       // link up AND the phone's capture service ready
    bool pending_execution = false;  // an action was dispatched and is unconfirmed
    // The picture the user chose on the phone, once one is stored and it matches
    // the current orientation. Null until then, and WALLPAPER says the name
    // instead: a blank panel reads as a dead device.
    const Icon* wallpaper = nullptr;
};

// Fills `out` for a state the device owns, and returns true. Returns false for
// the twelve the phone owns, leaving `out` untouched: their words arrive over
// BLE and the device must not invent any.
//
// The strings are static and some are built into a shared buffer, so `out` is
// valid until the next call. One caller, on the task that owns the display.
bool local_screen(State s, const DeviceStatus& st, ScreenModel& out);

// The device's own lines are written here rather than by the backend, so nothing
// else checks them against the budget. These are the limits a test holds them to:
// the narrower portrait width, so one set of words fits both orientations.
constexpr uint8_t kLocalMaxLineChars = 28;   // portrait body, 28 x 22
constexpr uint8_t kLocalMaxStatusChars = 40; // status and hint are the smaller face

}  // namespace halo
