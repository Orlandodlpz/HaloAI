// Gesture detector: one tactile side button plus a rotary switch, turned into the
// five V1 inputs. Timings come from Config, not from literals in here.
#include "halo/gestures.hpp"
#include "harness.hpp"

using namespace halo;
using namespace halo::test;
using E = halo::EventKind;

namespace {

struct GRig {
    Config cfg{};
    GestureDetector g{cfg};
    uint32_t now = 1000;

    GRig() = default;
    GRig(const GRig&) = delete;
    GRig& operator=(const GRig&) = delete;

    void down(uint32_t dt = 0) { now += dt; g.button_down(now); }
    void up(uint32_t dt = 0) { now += dt; g.button_up(now); }
    void turn(int32_t steps, uint32_t dt = 0) { now += dt; g.rotate(steps, now); }
    void wait(uint32_t dt) {
        // Tick the way the firmware loop will: often, not once.
        for (uint32_t i = 0; i < dt; i += 5) g.tick(now + i);
        now += dt;
        g.tick(now);
    }
    int drain(E* out, int max) {
        int n = 0;
        Event e;
        while (n < max && g.next(e)) out[n++] = e.kind;
        return n;
    }
};

}  // namespace

HALO_TEST(gesture_short_press_is_a_single_press_after_the_double_window) {
    GRig r;
    r.down();
    r.up(80);
    E got[4];
    CHECK(r.drain(got, 4) == 0);  // nothing yet: it could still become a double
    r.wait(r.cfg.double_press_ms + 10);
    CHECK(r.drain(got, 4) == 1);
    CHECK_EVENT(got[0], E::SinglePress);
}

HALO_TEST(gesture_two_quick_presses_are_a_double_press) {
    GRig r;
    r.down();
    r.up(60);
    r.wait(100);
    r.down(0);
    E got[4];
    const int n = r.drain(got, 4);
    CHECK(n == 1);
    CHECK_EVENT(got[0], E::DoublePress);
    r.up(40);
    r.wait(r.cfg.double_press_ms + 10);
    CHECK(r.drain(got, 4) == 0);  // the second release is swallowed
}

HALO_TEST(gesture_second_press_after_the_window_is_two_single_presses) {
    GRig r;
    r.down();
    r.up(60);
    r.wait(r.cfg.double_press_ms + 10);
    E got[4];
    CHECK(r.drain(got, 4) == 1);
    r.down(50);
    r.up(60);
    r.wait(r.cfg.double_press_ms + 10);
    CHECK(r.drain(got, 4) == 1);
    CHECK_EVENT(got[0], E::SinglePress);
}

HALO_TEST(gesture_long_press_is_a_hold_then_a_release) {
    GRig r;
    r.down();
    E got[4];
    r.wait(r.cfg.hold_ms - 50);
    CHECK(r.drain(got, 4) == 0);
    r.wait(100);
    CHECK(r.drain(got, 4) == 1);
    CHECK_EVENT(got[0], E::HoldStart);
    r.up(2000);
    CHECK(r.drain(got, 4) == 1);
    CHECK_EVENT(got[0], E::Release);
}

HALO_TEST(gesture_a_hold_never_also_produces_a_press) {
    // Spec global rule 7: a hold suppresses the click on release.
    GRig r;
    r.down();
    r.wait(r.cfg.hold_ms + 100);
    r.up();
    r.wait(r.cfg.double_press_ms + 50);
    E got[8];
    const int n = r.drain(got, 8);
    CHECK(n == 2);
    CHECK_EVENT(got[0], E::HoldStart);
    CHECK_EVENT(got[1], E::Release);
}

HALO_TEST(gesture_press_then_hold_reads_as_a_double_press) {
    // Deliberate: the second press down inside the window is reported as a double
    // press immediately, so the hold that follows is swallowed. A double press
    // cancels, which is recoverable; the alternative reading would be "approve,
    // then record", and approve is not recoverable. Flagged for the team.
    GRig r;
    r.down();
    r.up(60);
    r.wait(100);
    r.down();
    r.wait(r.cfg.hold_ms + 100);
    r.up();
    E got[8];
    const int n = r.drain(got, 8);
    CHECK(n == 1);
    CHECK_EVENT(got[0], E::DoublePress);
}

HALO_TEST(gesture_contact_bounce_is_ignored) {
    GRig r;
    r.down();
    r.up(2);    // bounce inside the debounce window
    r.down(3);  // bounce
    r.wait(r.cfg.hold_ms + 50);
    E got[4];
    const int n = r.drain(got, 4);
    CHECK(n == 1);
    CHECK_EVENT(got[0], E::HoldStart);  // one clean hold, not a press and a hold
}

HALO_TEST(gesture_rotation_reports_each_detent) {
    GRig r;
    r.turn(2);
    r.turn(-1, 10);
    E got[8];
    const int n = r.drain(got, 8);
    CHECK(n == 3);
    CHECK_EVENT(got[0], E::RotateUp);
    CHECK_EVENT(got[1], E::RotateUp);
    CHECK_EVENT(got[2], E::RotateDown);
}

HALO_TEST(gesture_hold_fires_while_the_button_is_still_down) {
    // RECORDING has to start during the hold, not on release, or the first words
    // of the command are lost.
    GRig r;
    r.down();
    r.wait(r.cfg.hold_ms + 5);
    E got[4];
    CHECK(r.drain(got, 4) == 1);
    CHECK_EVENT(got[0], E::HoldStart);
    CHECK(r.g.button_is_down());
}
