// A display backend that draws nothing, so layout can be tested on a computer.
// Configured with the metrics of whichever panel we want to pretend to be.
#pragma once

#include "halo/display.hpp"

namespace halo::test {

class FakeDisplay : public Display {
public:
    // The badge: 800 x 480, all of it visible, 4 gray levels.
    static FakeDisplay badge() { return FakeDisplay(Caps{800, 480, 480, 4, true}); }
    // The development panel: same logical canvas, but only 272 units of it on glass.
    static FakeDisplay crowpanel() { return FakeDisplay(Caps{800, 480, 272, 2, true}); }

    explicit FakeDisplay(const Caps& c) : caps_(c) {}

    bool init() override { return true; }
    Caps caps() const override { return caps_; }

    TextMetrics metrics(TextStyle style) const override {
        switch (style) {
            case TextStyle::Title: return TextMetrics{16, 32};
            case TextStyle::Status: return TextMetrics{10, 20};
            default: return TextMetrics{12, 24};
        }
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
