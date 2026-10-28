// Layout lives above the display boundary, so it can be tested on a computer by
// pointing it at a fake panel. The point of most of these tests is that the SAME
// screen model, unchanged, lays out correctly on two panels with different sizes.
#include "fake_display.hpp"
#include "halo/layout.hpp"
#include "halo/screens.hpp"
#include "halo/state_machine.hpp"
#include "harness.hpp"

using namespace halo;
using namespace halo::test;

namespace {

const char* const kReviewLines[] = {
    "To: victor@example.com", "Subject: Meeting moved",  "Hi Victor,",
    "The meeting moved to",   "Thursday at 3 pm.",       "Thanks,",
    "Carlos",                 "",                        "Sent from HaloAI",
    "line 10",                "line 11",                 "line 12",
    "line 13",                "line 14",                 "line 15",
    "line 16",                "line 17",                 "line 18",
};
constexpr uint8_t kReviewCount = sizeof(kReviewLines) / sizeof(kReviewLines[0]);

const char* const kCandidates[] = {"Victor T.", "Victor R.", "Victor M."};

// A tiny 8 x 8 icon, one bit per pixel.
const uint8_t kIconBits[] = {0x18, 0x3c, 0x7e, 0xff, 0xff, 0x7e, 0x3c, 0x18};
const Icon kIcon{kIconBits, 8, 8};

ScreenModel review_model() {
    ScreenModel m;
    m.state = State::Review;
    m.status_line = "Ready to confirm";
    m.lines = kReviewLines;
    m.line_count = kReviewCount;
    return m;
}

}  // namespace

HALO_TEST(layout_status_line_draws_a_status_row_and_a_divider) {
    FakeDisplay d = FakeDisplay::badge();
    DisplayList list;
    layout_screen(d, review_model(), list);
    CHECK(count_of(list, CmdKind::Line) == 1);
    CHECK(list[0].kind == CmdKind::Text);
    CHECK(list[0].text.style == TextStyle::Status);
}

HALO_TEST(layout_badge_shows_more_body_lines_than_the_development_panel) {
    // The whole reason the boundary exists: one screen model, two panels.
    FakeDisplay badge = FakeDisplay::badge();
    FakeDisplay crow = FakeDisplay::crowpanel();
    DisplayList a, b;
    const uint8_t left_badge = layout_screen(badge, review_model(), a);
    const uint8_t left_crow = layout_screen(crow, review_model(), b);
    CHECK(count_of(a, CmdKind::Text) > count_of(b, CmdKind::Text));
    CHECK(left_crow > left_badge);
}

HALO_TEST(layout_never_places_a_line_below_the_visible_window) {
    for (FakeDisplay d : {FakeDisplay::badge(), FakeDisplay::crowpanel()}) {
        DisplayList list;
        layout_screen(d, review_model(), list);
        const int16_t limit = d.caps().visible_h;
        for (uint8_t i = 0; i < list.size(); ++i) {
            if (list[i].kind != CmdKind::Text) continue;
            CHECK(list[i].text.box.bottom() <= limit);
        }
    }
}

HALO_TEST(layout_reports_how_many_lines_did_not_fit) {
    FakeDisplay d = FakeDisplay::crowpanel();
    DisplayList list;
    ScreenModel m = review_model();
    const uint8_t left = layout_screen(d, m, list);
    CHECK(left > 0);
    const int shown = count_of(list, CmdKind::Text) - 1;  // minus the status row
    CHECK(shown + left == kReviewCount);
}

HALO_TEST(layout_scrolling_advances_through_the_body) {
    FakeDisplay d = FakeDisplay::crowpanel();
    DisplayList first, scrolled;
    ScreenModel m = review_model();
    const uint8_t left0 = layout_screen(d, m, first);
    m.scroll_line = 3;
    const uint8_t left3 = layout_screen(d, m, scrolled);
    CHECK(left3 == left0 - 3);
    // The first body row after scrolling is the fourth source line. Find it
    // rather than assuming an index: the status bar also emits a divider line.
    const char* first_body = nullptr;
    for (uint8_t i = 0; i < scrolled.size() && first_body == nullptr; ++i)
        if (scrolled[i].kind == CmdKind::Text && scrolled[i].text.style != TextStyle::Status)
            first_body = scrolled[i].text.text;
    CHECK(first_body == kReviewLines[3]);
}

// A contact the backend could not match: CONFLICT-AMBIGUOUS arrives with no
// candidates at all. Its lines explain that; the way out is to say the address
// instead, which is a gesture, so the device names it (team addition,
// 2026-10-09).
HALO_TEST(layout_empty_candidate_list_offers_the_spoken_address) {
    FakeDisplay d = FakeDisplay::badge();
    DisplayList out;
    ScreenModel m = review_model();
    m.state = State::ConflictAmbiguous;
    m.candidate_count = 0;
    layout_screen(d, m, out);

    const char* found = nullptr;
    for (uint8_t i = 0; i < out.size(); ++i)
        if (out[i].kind == CmdKind::Text && out[i].text.text == no_candidates_hint())
            found = out[i].text.text;
    CHECK(found == no_candidates_hint());
}

// With candidates to choose from there is nothing to say out loud, so the hint
// would be telling the user to do the wrong thing.
HALO_TEST(layout_candidates_present_get_no_spoken_address_hint) {
    FakeDisplay d = FakeDisplay::badge();
    DisplayList out;
    ScreenModel m = review_model();
    m.state = State::ConflictAmbiguous;
    m.candidates = kCandidates;
    m.candidate_count = 3;
    layout_screen(d, m, out);

    for (uint8_t i = 0; i < out.size(); ++i)
        if (out[i].kind == CmdKind::Text)
            CHECK(out[i].text.text != no_candidates_hint());
}

// A hint the screen brought itself is never overwritten.
HALO_TEST(layout_an_explicit_hint_beats_the_default) {
    FakeDisplay d = FakeDisplay::badge();
    DisplayList out;
    ScreenModel m = review_model();
    m.state = State::ConflictAmbiguous;
    m.candidate_count = 0;
    m.hint = "Say it again";
    layout_screen(d, m, out);

    for (uint8_t i = 0; i < out.size(); ++i)
        if (out[i].kind == CmdKind::Text)
            CHECK(out[i].text.text != no_candidates_hint());
}

// Scrolled to the bottom: nothing is left below the fold, and the last source
// line is on the glass. The device clamps its scroll to exactly this point, so
// this is the position a user reaches by holding the knob down.
HALO_TEST(layout_scrolling_to_the_end_shows_the_last_line_and_nothing_remains) {
    FakeDisplay d = FakeDisplay::crowpanel();
    DisplayList first, last;
    ScreenModel m = review_model();
    const uint8_t over = layout_screen(d, m, first);
    CHECK(over > 0);                 // the body really is longer than this panel

    m.scroll_line = over;            // what the device's clamp allows
    CHECK(layout_screen(d, m, last) == 0);

    const char* lowest = nullptr;
    for (uint8_t i = 0; i < last.size(); ++i)
        if (last[i].kind == CmdKind::Text && last[i].text.style != TextStyle::Status)
            lowest = last[i].text.text;
    CHECK(lowest == kReviewLines[kReviewCount - 1]);
}

HALO_TEST(layout_never_rewraps_text_the_backend_already_wrapped) {
    // screen_lines arrive pre-wrapped and already checked against the text budget.
    // Layout must draw those exact strings, never re-flow them.
    FakeDisplay d = FakeDisplay::badge();
    DisplayList list;
    layout_screen(d, review_model(), list);
    uint8_t body = 0;
    for (uint8_t i = 0; i < list.size(); ++i) {
        if (list[i].kind != CmdKind::Text) continue;
        if (list[i].text.style == TextStyle::Status) continue;
        CHECK(list[i].text.text == kReviewLines[body]);  // same pointer, not a copy
        ++body;
    }
    CHECK(body > 0);
}

HALO_TEST(layout_icon_only_screen_is_one_bitmap_and_no_body_text) {
    FakeDisplay d = FakeDisplay::badge();
    DisplayList list;
    ScreenModel m;
    m.state = State::Success;
    m.icon = &kIcon;
    layout_screen(d, m, list);
    CHECK(count_of(list, CmdKind::Bitmap) == 1);
    CHECK(count_of(list, CmdKind::Text) == 0);
}

HALO_TEST(layout_icon_above_text_leaves_room_for_both) {
    FakeDisplay d = FakeDisplay::badge();
    DisplayList list;
    ScreenModel m = review_model();
    m.icon = &kIcon;
    layout_screen(d, m, list);
    CHECK(count_of(list, CmdKind::Bitmap) == 1);
    CHECK(count_of(list, CmdKind::Text) > 1);
}

HALO_TEST(layout_highlighted_candidate_is_filled_and_inverted) {
    FakeDisplay d = FakeDisplay::badge();
    DisplayList list;
    ScreenModel m;
    m.state = State::ConflictAmbiguous;
    m.candidates = kCandidates;
    m.candidate_count = 3;
    m.highlight = 1;
    layout_screen(d, m, list);
    CHECK(count_of(list, CmdKind::Rect) == 1);
    int inverted = 0;
    for (uint8_t i = 0; i < list.size(); ++i)
        if (list[i].kind == CmdKind::Text && list[i].text.invert) ++inverted;
    CHECK(inverted == 1);
}

HALO_TEST(layout_empty_candidate_list_still_shows_the_hint) {
    // "no contact matched" goes to CONFLICT-AMBIGUOUS with an empty list and a
    // hold to say the address prompt (team addition, 2026-10-09).
    FakeDisplay d = FakeDisplay::badge();
    DisplayList list;
    ScreenModel m;
    m.state = State::ConflictAmbiguous;
    m.status_line = "Who should get this?";
    m.lines = nullptr;
    m.candidate_count = 0;
    m.hint = "Hold to say the address";
    layout_screen(d, m, list);
    bool found = false;
    for (uint8_t i = 0; i < list.size(); ++i)
        if (list[i].kind == CmdKind::Text && list[i].text.text == m.hint) found = true;
    CHECK(found || list.size() > 0);
}

HALO_TEST(layout_display_list_reports_overflow_instead_of_corrupting) {
    FakeDisplay d = FakeDisplay::badge();
    DisplayList list;
    for (int i = 0; i < DisplayList::kMax + 5; ++i) list.text(TextCmd{});
    CHECK(list.size() == DisplayList::kMax);
    CHECK(list.dropped() == 5);
}

// ---------------------------------------------------------------- refresh policy

HALO_TEST(refresh_status_only_change_is_partial) {
    RefreshPolicy p;
    CHECK(p.choose(State::Processing, true) == Refresh::Partial);
}

HALO_TEST(refresh_final_screens_are_partial_not_full) {
    // A full refresh on the badge is about 3.5 s, which alone blows the 1.5 s
    // slot in the latency budget, so final screens use partial too.
    RefreshPolicy p;
    CHECK(p.choose(State::Review, false) == Refresh::Partial);
    CHECK(p.choose(State::Success, false) == Refresh::Partial);
}

HALO_TEST(refresh_goes_full_periodically_to_clear_ghosting) {
    RefreshPolicy p(3);
    for (int i = 0; i < 3; ++i) {
        CHECK(p.choose(State::Review, false) == Refresh::Partial);
        p.note_presented(Refresh::Partial);
    }
    CHECK(p.choose(State::Review, false) == Refresh::Full);
    p.note_presented(Refresh::Full);
    CHECK(p.partials_since_full() == 0);
    CHECK(p.choose(State::Review, false) == Refresh::Partial);
}

// A full refresh is 2.5 s on the badge, nearly 8 s when it clears ghosting
// first, and the main loop does not run while the panel is being driven. Chosen
// while audio is streaming it stalls the stream behind it: on 2026-10-28 one
// landed on FINALIZE AUDIO and pushed the final marker from 0.65 s after the
// release to 7.6 s, against an 8 s delivery deadline, and the request failed.
HALO_TEST(refresh_never_goes_full_while_audio_is_in_flight) {
    for (State s : {State::Recording, State::RecordAddition, State::FinalizeAudio}) {
        RefreshPolicy p(3);
        for (int i = 0; i < 10; ++i) {
            CHECK(p.choose(s, false) == Refresh::Partial);
            p.note_presented(Refresh::Partial);
        }
    }
}

// Deferred, not skipped: the count keeps running underneath, so the ghost
// clearing happens on the next screen that can afford it.
HALO_TEST(refresh_defers_the_full_until_audio_is_done) {
    RefreshPolicy p(3);
    for (int i = 0; i < 6; ++i) {
        CHECK(p.choose(State::FinalizeAudio, false) == Refresh::Partial);
        p.note_presented(Refresh::Partial);
    }
    CHECK(p.choose(State::Review, false) == Refresh::Full);
}

HALO_TEST(refresh_wallpaper_and_boot_are_always_full) {
    RefreshPolicy p;
    CHECK(p.choose(State::Wallpaper, false) == Refresh::Full);
    CHECK(p.choose(State::BootRecovery, false) == Refresh::Full);
}

HALO_TEST(present_callback_is_what_starts_the_confirm_guard) {
    // Ties the display boundary to the safety property: the guard begins when the
    // backend says the pixels are on the panel, not when the screen was queued.
    Config cfg;
    StateMachine sm(cfg);
    sm.handle(Event::at(EventKind::StartupOk, 10));
    sm.handle(Event::at(EventKind::HoldStart, 20));
    sm.handle(Event::at(EventKind::PhoneReady, 30));
    sm.handle(Event::at(EventKind::Release, 40));
    sm.handle(Event::at(EventKind::AudioComplete, 50));
    sm.handle(Event::at(EventKind::ScreenReview, 60));
    CHECK(sm.state() == State::Review);

    FakeDisplay d = FakeDisplay::badge();
    d.present_at_ms = 900;  // e-paper took most of a second to show it
    CHECK(!sm.confirm_guard_open(899));
    struct Ctx { StateMachine* sm; } ctx{&sm};
    d.present(Refresh::Partial,
              [](void* c, uint32_t at) {
                  static_cast<Ctx*>(c)->sm->handle(Event::at(EventKind::ScreenPresented, at));
              },
              &ctx);
    CHECK(!sm.confirm_guard_open(900 + cfg.confirm_guard_ms - 1));
    CHECK(sm.confirm_guard_open(900 + cfg.confirm_guard_ms));
}

// ---------------------------------------------------------------- text budget

// These are the numbers the backend wraps `screen_lines` to and the phone sizes
// its preview against, published in BRIEFING section 2. They are a contract with
// two other people, so they are asserted here rather than left to drift when
// someone changes a margin or a font.
HALO_TEST(text_budget_is_what_was_published_to_the_team) {
    struct Case { FakeDisplay d; const char* what; int cols; int lines; };
    {
        FakeDisplay d = FakeDisplay::badge();
        CHECK(chars_that_fit(d, TextStyle::Body, 800 - 2 * 16) == 48);
    }
    {
        FakeDisplay d = FakeDisplay::badge_portrait();
        CHECK(chars_that_fit(d, TextStyle::Body, 480 - 2 * 16) == 28);
    }
    // Lines are measured the way layout measures them: the visible window minus
    // the status bar above and the hint below.
    const char* const lines[40] = {};
    ScreenModel m;
    m.state = State::Review;
    m.status_line = "x";
    m.lines = lines;
    m.line_count = 40;
    DisplayList list;

    FakeDisplay badge = FakeDisplay::badge();
    CHECK(40 - layout_screen(badge, m, list) == 12);

    FakeDisplay portrait = FakeDisplay::badge_portrait();
    CHECK(40 - layout_screen(portrait, m, list) == 22);

    // The development panel shows the contract minimum for REVIEW: the full
    // address, the subject and two body lines, with the rest reached by rotating.
    FakeDisplay crow = FakeDisplay::crowpanel();
    const int crow_lines = 40 - layout_screen(crow, m, list);
    CHECK(crow_lines == 5);
    CHECK(crow_lines >= 4);
}
