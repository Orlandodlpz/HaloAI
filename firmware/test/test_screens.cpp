// The words on the nine screens the device owns.
//
// The backend's screen_lines are checked against the text budget before they are
// sent. Nothing checks the device's own lines, so these tests do: they hold every
// local screen to the narrower portrait budget, so one set of words fits both
// orientations, and they pin which states the device is allowed to write at all.
#include <cstring>
#include <string>

#include "fake_display.hpp"
#include "halo/screens.hpp"
#include "harness.hpp"

using namespace halo;

namespace {

// The nine the device owns (BRIEFING section 2, "State ownership").
const State kLocal[] = {
    State::BootRecovery, State::Ready,          State::Wallpaper,
    State::PhoneCheck,   State::PhoneDisconnected, State::Recording,
    State::RecordAddition, State::FinalizeAudio, State::CriticalBattery,
};

bool is_local(State s) {
    for (State l : kLocal)
        if (l == s) return true;
    return false;
}

}  // namespace

HALO_TEST(screens_the_device_writes_exactly_the_nine_it_owns) {
    // A screen the phone owns must never get words from here: the backend wrote
    // and checked that text, and anything else on the glass is text nobody
    // approved. The other direction leaves a state with nothing to draw.
    DeviceStatus st;
    for (uint8_t i = 0; i < kStateCount; ++i) {
        const State s = static_cast<State>(i);
        ScreenModel m;
        const bool wrote = local_screen(s, st, m);
        if (wrote != is_local(s))
            ::halo::test::fail(__FILE__, __LINE__,
                               std::string(name(s)) + ": local_screen returned " +
                                   (wrote ? "true" : "false"));
    }
}

HALO_TEST(screens_every_local_line_fits_the_narrower_orientation) {
    // 28 characters is the portrait body budget, the smaller of the two. Holding
    // the device's own words to it means one set of words works in both.
    DeviceStatus st;
    st.pending_execution = true;  // the longest variant of every screen
    st.battery_pct = 100;         // and the longest battery number
    for (State s : kLocal) {
        ScreenModel m;
        CHECK(local_screen(s, st, m));
        for (uint8_t i = 0; i < m.line_count; ++i) {
            const size_t n = strlen(m.lines[i]);
            if (n > kLocalMaxLineChars)
                ::halo::test::fail(__FILE__, __LINE__,
                                   std::string(name(s)) + " line " + std::to_string(i) + " is " +
                                       std::to_string(n) + " characters: \"" + m.lines[i] + "\"");
        }
        if (m.status_line != nullptr && strlen(m.status_line) > kLocalMaxStatusChars)
            ::halo::test::fail(__FILE__, __LINE__,
                               std::string(name(s)) + " status line is too long: " + m.status_line);
        if (m.hint != nullptr && strlen(m.hint) > kLocalMaxStatusChars)
            ::halo::test::fail(__FILE__, __LINE__,
                               std::string(name(s)) + " hint is too long: " + m.hint);
    }
}

HALO_TEST(screens_every_local_screen_says_something) {
    // A state that draws nothing looks like a device that died. Every one of the
    // nine has to put at least one line on the glass.
    DeviceStatus st;
    for (State s : kLocal) {
        ScreenModel m;
        CHECK(local_screen(s, st, m));
        CHECK(m.line_count > 0);
        CHECK(m.lines != nullptr);
        for (uint8_t i = 0; i < m.line_count; ++i) CHECK(m.lines[i][0] != '\0');
    }
}

HALO_TEST(screens_a_hint_is_only_drawn_when_there_are_lines_to_hang_it_on) {
    // layout_screen draws the hint inside the body branches, so a screen with a
    // hint and no lines would silently lose it.
    DeviceStatus st;
    for (State s : kLocal) {
        ScreenModel m;
        CHECK(local_screen(s, st, m));
        if (m.hint != nullptr) CHECK(m.line_count > 0);
    }
}

HALO_TEST(screens_critical_battery_shows_the_number_and_the_pending_action) {
    DeviceStatus st;
    st.battery_pct = 7;
    ScreenModel m;
    CHECK(local_screen(State::CriticalBattery, st, m));
    CHECK(std::string(m.lines[0]) == "Battery at 7%");
    CHECK(m.line_count == 2);

    st.pending_execution = true;
    CHECK(local_screen(State::CriticalBattery, st, m));
    CHECK(m.line_count == 3);
    CHECK(std::string(m.lines[2]).find("unconfirmed") != std::string::npos);

    st.battery_pct = 100;
    CHECK(local_screen(State::CriticalBattery, st, m));
    CHECK(std::string(m.lines[0]) == "Battery at 100%");
}

HALO_TEST(screens_ready_says_whether_the_phone_is_there) {
    DeviceStatus st;
    ScreenModel linked_off;
    CHECK(local_screen(State::Ready, st, linked_off));
    const std::string without = linked_off.status_line;

    st.phone_linked = true;
    ScreenModel linked_on;
    CHECK(local_screen(State::Ready, st, linked_on));
    CHECK(without != std::string(linked_on.status_line));
}

HALO_TEST(screens_the_wallpaper_has_no_status_bar) {
    // It is the whole screen by definition. A status bar across the top of
    // someone's chosen picture is not a wallpaper.
    DeviceStatus st;
    ScreenModel m;
    CHECK(local_screen(State::Wallpaper, st, m));
    CHECK(m.status_line == nullptr);
}

HALO_TEST(screens_local_screens_lay_out_on_every_panel_we_have) {
    // The real check that the words fit: run them through the layout and make
    // sure nothing falls off the bottom. The development panel is the short one
    // (272 of 480), and portrait is the narrow one, so between them they catch
    // both ways a line can be too big.
    DeviceStatus st;
    st.pending_execution = true;
    DisplayList list;
    struct Panel { const char* name; test::FakeDisplay d; };
    Panel panels[] = {
        {"badge landscape", test::FakeDisplay::badge()},
        {"badge portrait", test::FakeDisplay::badge_portrait()},
        {"development board", test::FakeDisplay::crowpanel()},
    };
    for (Panel& p : panels) {
        for (State s : kLocal) {
            ScreenModel m;
            CHECK(local_screen(s, st, m));
            const uint8_t overflow = layout_screen(p.d, m, list);
            if (overflow != 0)
                ::halo::test::fail(__FILE__, __LINE__,
                                   std::string(p.name) + ", " + name(s) + ": overflows by " +
                                       std::to_string(overflow) + " lines");
            if (list.dropped() != 0)
                ::halo::test::fail(__FILE__, __LINE__,
                                   std::string(p.name) + ", " + name(s) + ": display list full");
        }
    }
}
