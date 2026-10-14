// A display backend that draws nothing, so layout can be tested on a computer.
// Configured with the metrics of whichever panel we want to pretend to be.
#pragma once

#include "halo/display.hpp"

namespace halo::test {

class FakeDisplay : public Display {
public:
    // The badge: 800 x 480, all of it visible, 4 gray levels.
    static FakeDisplay badge() { return FakeDisplay(Caps{800, 480, 480, 4, true}); }
    // The badge turned 90 degrees, which is a setting the phone sends.
    static FakeDisplay badge_portrait() { return FakeDisplay(Caps{480, 800, 800, 2, true}); }
    // The development panel: same logical canvas, but only 272 units of it on glass.
    static FakeDisplay crowpanel() { return FakeDisplay(Caps{800, 480, 272, 2, true}); }

    explicit FakeDisplay(const Caps& c) : caps_(c) {}

    bool init() override { return true; }
    Caps caps() const override { return caps_; }

    // Mirrors the real backends: Spleen 16x32 for screen_lines, 12x24 for the
    // status bar. Kept in step by hand; the glyph data itself lives in the
    // backends, below the display boundary.
    TextMetrics metrics(TextStyle style) const override {
        if (style == TextStyle::Status) return TextMetrics{12, 24};
        return TextMetrics{16, 32};
    }

    void clear() override { ++clears; }
    void draw(const DisplayList& list) override { last = list; ++draws; }
    void present(Refresh intent, PresentedFn on_presented, void* ctx) override {
        last_intent = intent;
        ++presents;
        // A real e-paper backend calls this hundreds of ms later. Tests that care
        // about the confirm guard drive the timing themselves.
        if (on_presented) on_presented(ctx, present_at_ms);
    }
    void sleep() override { ++sleeps; }

    Caps caps_;
    DisplayList last{};
    Refresh last_intent = Refresh::Partial;
    uint32_t present_at_ms = 0;
    int clears = 0, draws = 0, presents = 0, sleeps = 0;
};

inline int count_of(const DisplayList& l, CmdKind k) {
    int n = 0;
    for (uint8_t i = 0; i < l.size(); ++i)
        if (l[i].kind == k) ++n;
    return n;
}

}  // namespace halo::test
