// Screen layout: a screen model in, a display list out. Lives above the display
// boundary, so it has no idea what panel it is drawing on. It asks the backend for
// text metrics and lays out in logical units.
//
// Testable on a computer: point it at a fake display with whatever metrics you
// like and inspect the display list it produces.
#pragma once

#include "halo/display.hpp"
#include "halo/states.hpp"

namespace halo {

struct Icon {
    const uint8_t* bits = nullptr;  // 1 bit per pixel, rows byte padded, set bit = ink
    uint16_t w = 0, h = 0;
    // The wallpaper is the exception and it is not ours to change: the phone, both
    // panels' framebuffers and the phone's preview all use 1 = white. It is stored
    // as it arrived so those bytes stay comparable, and inverted here instead.
    bool invert = false;
};

// Everything a screen can show. The phone fills most of it from the backend's
// result; the device fills it itself for its local states.
struct ScreenModel {
    State state = State::Ready;
    const char* status_line = nullptr;   // status bar, partial refresh region

    const char* const* lines = nullptr;  // pre-wrapped by the backend, never re-wrapped here
    uint8_t line_count = 0;

    const char* const* candidates = nullptr;  // CONFLICT-AMBIGUOUS, display order
    uint8_t candidate_count = 0;
    int8_t highlight = -1;               // index of the highlighted candidate

    const Icon* icon = nullptr;          // a screen that is just an icon, or an icon plus lines
    const char* hint = nullptr;          // bottom line, for example "hold to say the address"

    uint16_t expires_in_s = 0;           // decision states: shown in the status bar
    uint8_t scroll_line = 0;             // first body line to show, for rotate to scroll

    // Right hand end of the status bar. -1 means the device does not know yet,
    // which is the ordinary state on USB power with no cell on the connector,
    // and nothing is drawn rather than claiming 0%.
    int8_t battery_pct = -1;
    bool charging = false;
};

// Fills `out` with the screen. Returns the number of body lines that did not fit,
// so the caller knows whether rotating can scroll further.
uint8_t layout_screen(const Display& d, const ScreenModel& m, DisplayList& out);

// Which refresh a present should use.
//
// On the badge a full refresh is about 3.5 s, which alone blows the 1.5 s slot in
// the latency budget, so final screens use partial too and ghosting is cleared by
// a periodic full refresh instead.
class RefreshPolicy {
public:
    explicit RefreshPolicy(uint8_t full_every = 10) : full_every_(full_every) {}

    // status_only: just the status bar changed, body untouched.
    Refresh choose(State to, bool status_only);
    void note_presented(Refresh used);
    uint8_t partials_since_full() const { return partials_since_full_; }

private:
    uint8_t full_every_;
    uint8_t partials_since_full_ = 0;
};

}  // namespace halo
