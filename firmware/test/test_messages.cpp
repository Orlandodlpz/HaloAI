// The BLE message layer against the agreed contract (BRIEFING section 2, detail
// in phone/docs/ble_fields_draft.md and firmware/docs/ble_fields.md).
//
// These tests compare whole messages byte for byte rather than field by field.
// The phone parses bytes, so a test that only checks "the sid is there" would
// pass while the device sent something the phone cannot read.
#include <cstring>
#include <string>
#include <vector>

#include "halo/messages.hpp"
#include "harness.hpp"

using namespace halo;

namespace {

std::string built(size_t (*fn)(char*, size_t, const msg::Link&), const msg::Link& l) {
    char buf[256];
    const size_t n = fn(buf, sizeof(buf), l);
    return n == 0 ? std::string("<overflow>") : std::string(buf, n);
}

#define CHECK_JSON(actual, expected)                                                    \
    do {                                                                                \
        const std::string a_ = (actual);                                                \
        const std::string e_ = (expected);                                              \
        if (a_ != e_)                                                                   \
            ::halo::test::fail(__FILE__, __LINE__, "\n    expected " + e_ + "\n    got      " + a_); \
    } while (0)

msg::Link demo_link() {
    msg::Link l;
    l.req = 12;
    l.seg = 3;
    l.rev = 2;
    l.sid = 42;
    l.battery = 87;
    return l;
}

}  // namespace

// ------------------------------------------------------------- state spelling

HALO_TEST(messages_wire_names_are_the_spec_names_mechanically_transformed) {
    // The wire spelling is a rule, not a list, so Carlos can derive the same
    // table from BRIEFING section 2 without reading our source: upper case, every
    // run of characters that are not letters or digits becomes one underscore.
    // A hand written table that drifts from that rule is the bug this catches.
    for (uint8_t i = 0; i < kStateCount; ++i) {
        const State s = static_cast<State>(i);
        std::string want;
        bool pending_underscore = false;
        for (const char* p = name(s); *p; ++p) {
            const bool alnum = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                               (*p >= '0' && *p <= '9');
            if (alnum) {
                if (pending_underscore && !want.empty()) want += '_';
                pending_underscore = false;
                want += static_cast<char>(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
            } else {
                pending_underscore = true;
            }
        }
        if (want != msg::wire_name(s))
            ::halo::test::fail(__FILE__, __LINE__,
                               std::string("state ") + name(s) + ": rule says " + want +
                                   ", table says " + msg::wire_name(s));
    }
}

HALO_TEST(messages_wire_names_round_trip_and_reject_junk) {
    for (uint8_t i = 0; i < kStateCount; ++i) {
        const State s = static_cast<State>(i);
        State back = State::Ready;
        CHECK(msg::state_from_wire(msg::wire_name(s), &back));
        CHECK(back == s);
    }
    State unused = State::Ready;
    CHECK(!msg::state_from_wire("REVIEWED", &unused));
    CHECK(!msg::state_from_wire("review", &unused));  // the contract says upper case
    CHECK(!msg::state_from_wire("", &unused));
}

// ------------------------------------------------------------------ builders

HALO_TEST(messages_hello_carries_everything_the_phone_resyncs_from) {
    msg::Link l = demo_link();
    l.sid = -1;  // nothing drawn yet
    char buf[256];
    const size_t n = msg::hello(buf, sizeof(buf), l, "f1-dev", 3735928559u, State::Ready);
    // boot is a full u32: 0xDEADBEEF would come out negative through an int32.
    CHECK_JSON(std::string(buf, n),
               R"({"type":"hello","fw":"f1-dev","boot":3735928559,"state":"READY","req":12,)"
               R"("sid":null,"pending_key":null,"battery":87})");
}

HALO_TEST(messages_hello_sends_a_pending_key_when_there_is_one) {
    msg::Link l = demo_link();
    std::memcpy(l.pending_key, "01HQZX9K3M7N2P4R6T8V0W1Y3Z", 27);
    char buf[256];
    const size_t n = msg::hello(buf, sizeof(buf), l, "f1-dev", 1, State::ExecutionUncertain);
    CHECK(std::string(buf, n).find(R"("pending_key":"01HQZX9K3M7N2P4R6T8V0W1Y3Z")") !=
          std::string::npos);
    CHECK(std::string(buf, n).find(R"("state":"EXECUTION_UNCERTAIN")") != std::string::npos);
}

HALO_TEST(messages_approve_select_continue_echo_the_screen_they_acted_on) {
    const msg::Link l = demo_link();
    CHECK_JSON(built(msg::approve, l), R"({"type":"approve","sid":42})");
    CHECK_JSON(built(msg::continue_, l), R"({"type":"continue","sid":42})");
    char buf[128];
    const size_t n = msg::select(buf, sizeof(buf), l, 2);
    CHECK_JSON(std::string(buf, n), R"({"type":"select","sid":42,"index":2})");
}

HALO_TEST(messages_recording_events_carry_the_segment_identity) {
    const msg::Link l = demo_link();
    char buf[128];
    size_t n = msg::record_start(buf, sizeof(buf), l, false);
    CHECK_JSON(std::string(buf, n),
               R"({"type":"record_start","req":12,"seg":3,"rev":2,"addition":false})");
    n = msg::record_start(buf, sizeof(buf), l, true);
    CHECK(std::string(buf, n).find(R"("addition":true)") != std::string::npos);
    n = msg::record_end(buf, sizeof(buf), l, 233, 4660);
    CHECK_JSON(std::string(buf, n),
               R"({"type":"record_end","req":12,"seg":3,"chunks":233,"ms":4660})");
}

HALO_TEST(messages_cancel_always_says_why) {
    // Without the reason the phone cannot tell "the user changed their mind" from
    // "the device gave up", which is the whole point of the field.
    const msg::Link l = demo_link();
    char buf[128];
    size_t n = msg::cancel(buf, sizeof(buf), l, msg::CancelReason::User);
    CHECK_JSON(std::string(buf, n), R"({"type":"cancel","req":12,"sid":42,"reason":"user"})");
    n = msg::cancel(buf, sizeof(buf), l, msg::CancelReason::Expired);
    CHECK(std::string(buf, n).find(R"("reason":"expired")") != std::string::npos);
    n = msg::cancel(buf, sizeof(buf), l, msg::CancelReason::Transport);
    CHECK(std::string(buf, n).find(R"("reason":"transport")") != std::string::npos);
    n = msg::cancel(buf, sizeof(buf), l, msg::CancelReason::Battery);
    CHECK(std::string(buf, n).find(R"("reason":"battery")") != std::string::npos);
}

HALO_TEST(messages_cancel_before_any_screen_sends_a_null_sid) {
    // Cancelling in PHONE / BLUETOOTH CHECK happens before anything was drawn.
    // null is not the same as 0, which is a real screen ID.
    msg::Link l = demo_link();
    l.sid = -1;
    char buf[128];
    const size_t n = msg::cancel(buf, sizeof(buf), l, msg::CancelReason::User);
    CHECK_JSON(std::string(buf, n), R"({"type":"cancel","req":12,"sid":null,"reason":"user"})");
}

HALO_TEST(messages_the_small_events_match_the_contract) {
    const msg::Link l = demo_link();
    char buf[128];
    size_t n = msg::wake(buf, sizeof(buf));
    CHECK_JSON(std::string(buf, n), R"({"type":"wake"})");
    CHECK_JSON(built(msg::ack, l), R"({"type":"ack","sid":42})");
    CHECK_JSON(built(msg::activity, l), R"({"type":"activity","sid":42})");
    CHECK_JSON(built(msg::dismiss, l), R"({"type":"dismiss","sid":42,"pending_key":null})");
    CHECK_JSON(built(msg::status_query, l), R"({"type":"status_query","pending_key":null})");
    n = msg::battery(buf, sizeof(buf), 9);
    CHECK_JSON(std::string(buf, n), R"({"type":"battery","battery":9})");
}

HALO_TEST(messages_that_do_not_fit_report_zero_and_are_never_sent) {
    // A truncated object is not JSON. Half an approve is worse than no approve,
    // so the builder refuses rather than handing the radio something broken.
    msg::Link l = demo_link();
    std::memcpy(l.pending_key, "01HQZX9K3M7N2P4R6T8V0W1Y3Z", 27);
    char small[48];
    CHECK(msg::hello(small, sizeof(small), l, "f1-dev", 1, State::Ready) == 0);
    char tiny[8];
    CHECK(msg::approve(tiny, sizeof(tiny), l) == 0);
}

// ------------------------------------------------------------------ incoming

HALO_TEST(messages_reads_an_audio_ack_and_its_gap) {
    const char* plain = R"({"req":12,"seg":0,"upto":-1})";
    msg::AudioAck a;
    CHECK(msg::parse_audio_ack(plain, strlen(plain), &a));
    CHECK(a.upto == -1 && !a.has_resend && !a.final_segment);

    const char* gap = R"({"req":12,"seg":0,"upto":57,"resend_from":58})";
    CHECK(msg::parse_audio_ack(gap, strlen(gap), &a));
    CHECK(a.has_resend && a.resend_from == 58);

    const char* fin = R"({"req":12,"seg":0,"upto":140,"final":true})";
    CHECK(msg::parse_audio_ack(fin, strlen(fin), &a));
    CHECK(a.final_segment && a.upto == 140);

    const char* junk = R"({"req":12})";
    CHECK(!msg::parse_audio_ack(junk, strlen(junk), &a));
}

HALO_TEST(messages_reads_a_review_screen) {
    const char* doc =
        R"({"sid":42,"screen":"REVIEW","rev":3,)"
        R"("screen_lines":["To: victor@example.com","Subject: Meeting moved","Body line"],)"
        R"("status_line":"Ready to confirm","candidates":[],"expires_in_s":60})";
    msg::Screen s;
    CHECK(msg::parse_screen(doc, strlen(doc), &s));
    CHECK(s.sid == 42);
    CHECK(s.has_state && s.state == State::Review);
    CHECK(s.has_rev && s.rev == 3);
    CHECK(s.line_count == 3);
    CHECK(std::string(s.lines[0]) == "To: victor@example.com");
    CHECK(std::string(s.lines[2]) == "Body line");
    CHECK(s.line_ptrs[1] == s.lines[1]);  // handed to ScreenModel without a copy
    CHECK(s.candidate_count == 0);
    CHECK(std::string(s.status_line) == "Ready to confirm");
    CHECK(s.expires_in_s == 60);
    CHECK(s.has_body && !s.truncated);
}

HALO_TEST(messages_reads_screen_lines_containing_commas_quotes_and_brackets) {
    // Pre-wrapped email text is arbitrary user content. A reader that split the
    // array on commas would cut a line in half and the user would approve
    // something they never read.
    const char* doc =
        R"({"sid":7,"screen":"REVIEW","screen_lines":)"
        R"(["Lunch, then the gym","He said \"yes\"","array [1,2] in text","back\\slash"]})";
    msg::Screen s;
    CHECK(msg::parse_screen(doc, strlen(doc), &s));
    CHECK(s.line_count == 4);
    CHECK(std::string(s.lines[0]) == "Lunch, then the gym");
    CHECK(std::string(s.lines[1]) == "He said \"yes\"");
    CHECK(std::string(s.lines[2]) == "array [1,2] in text");
    CHECK(std::string(s.lines[3]) == "back\\slash");
}

HALO_TEST(messages_reads_candidates_in_display_order) {
    const char* doc =
        R"({"sid":8,"screen":"CONFLICT_AMBIGUOUS","screen_lines":["Which Victor?"],)"
        R"("candidates":["Victor Munoz <v@a.com>","Victor Lee <v@b.com>"]})";
    msg::Screen s;
    CHECK(msg::parse_screen(doc, strlen(doc), &s));
    CHECK(s.state == State::ConflictAmbiguous);
    CHECK(s.candidate_count == 2);
    CHECK(std::string(s.candidates[0]) == "Victor Munoz <v@a.com>");
    CHECK(std::string(s.candidates[1]) == "Victor Lee <v@b.com>");
    CHECK(s.candidate_ptrs[0] == s.candidates[0]);
}

HALO_TEST(messages_a_status_line_on_its_own_leaves_the_body_alone) {
    // This is the partial refresh path: 495 ms instead of a full redraw. If it
    // reported a body, every status update would blank the screen.
    const char* doc = R"({"sid":43,"status_line":"Searching the web"})";
    msg::Screen s;
    CHECK(msg::parse_screen(doc, strlen(doc), &s));
    CHECK(s.sid == 43);
    CHECK(!s.has_state);
    CHECK(!s.has_body);
    CHECK(s.has_status && std::string(s.status_line) == "Searching the web");
}

HALO_TEST(messages_an_unknown_screen_is_dropped_whole) {
    // A screen the device does not know is a contract mismatch. Drawing the half
    // of it that did parse would show the user a screen nobody designed.
    msg::Screen s;
    const char* good = R"({"sid":1,"screen":"REVIEW","screen_lines":["keep me"]})";
    CHECK(msg::parse_screen(good, strlen(good), &s));

    const char* bad = R"({"sid":2,"screen":"SOMETHING_NEW","screen_lines":["drop me"]})";
    CHECK(!msg::parse_screen(bad, strlen(bad), &s));
    CHECK(s.sid == 1);  // the previous screen is still intact
    CHECK(std::string(s.lines[0]) == "keep me");

    const char* no_sid = R"({"screen":"REVIEW"})";
    CHECK(!msg::parse_screen(no_sid, strlen(no_sid), &s));
    CHECK(s.sid == 1);
}

HALO_TEST(messages_too_many_lines_are_reported_not_hidden) {
    // More lines than the budget means the backend and the device disagree about
    // the text budget. That is a contract bug and it should be visible in the log,
    // not silently cropped.
    std::string doc = R"({"sid":3,"screen":"READ_ONLY_RESULT","screen_lines":[)";
    for (int i = 0; i < msg::kMaxLines + 4; ++i) doc += (i ? ",\"line\"" : "\"line\"");
    doc += "]}";
    msg::Screen s;
    CHECK(msg::parse_screen(doc.c_str(), doc.size(), &s));
    CHECK(s.line_count == msg::kMaxLines);
    CHECK(s.truncated);
}

HALO_TEST(messages_a_line_longer_than_the_budget_is_cut_not_overflowed) {
    std::string long_line(msg::kMaxLineChars + 20, 'x');
    const std::string doc =
        R"({"sid":4,"screen":"REVIEW","screen_lines":[")" + long_line + R"("]})";
    msg::Screen s;
    CHECK(msg::parse_screen(doc.c_str(), doc.size(), &s));
    CHECK(s.line_count == 1);
    CHECK(std::strlen(s.lines[0]) == msg::kMaxLineChars);
}

HALO_TEST(messages_a_failure_screen_means_different_things_while_executing) {
    // Both land in FAILURE, but only one of them is an authoritative "no effect",
    // and that decides whether the pending execution key may be cleared.
    EventKind e = EventKind::Tick;
    CHECK(msg::event_for_screen(State::Failure, State::Processing, &e));
    CHECK_EVENT(e, EventKind::ProcessingFailed);
    CHECK(msg::event_for_screen(State::Failure, State::Executing, &e));
    CHECK_EVENT(e, EventKind::ExecutionRejected);
    CHECK(msg::event_for_screen(State::Failure, State::ReconcileStatus, &e));
    CHECK_EVENT(e, EventKind::ExecutionRejected);
}

HALO_TEST(messages_every_decision_screen_maps_to_its_event) {
    EventKind e = EventKind::Tick;
    CHECK(msg::event_for_screen(State::Review, State::Processing, &e));
    CHECK_EVENT(e, EventKind::ScreenReview);
    CHECK(msg::event_for_screen(State::ConflictAmbiguous, State::Processing, &e));
    CHECK_EVENT(e, EventKind::ScreenConflictAmbiguous);
    CHECK(msg::event_for_screen(State::ConflictSchedule, State::Processing, &e));
    CHECK_EVENT(e, EventKind::ScreenConflictSchedule);
    CHECK(msg::event_for_screen(State::ReadOnlyResult, State::Processing, &e));
    CHECK_EVENT(e, EventKind::ScreenReadOnly);
    CHECK(msg::event_for_screen(State::ProcessingSlow, State::Processing, &e));
    CHECK_EVENT(e, EventKind::ProcessingSlow);
    CHECK(msg::event_for_screen(State::Success, State::Executing, &e));
    CHECK_EVENT(e, EventKind::ExecutionApplied);
    CHECK(msg::event_for_screen(State::ExecutionUncertain, State::Executing, &e));
    CHECK_EVENT(e, EventKind::ExecutionUnknown);

    // States the device owns, or that the V1 table has no event for.
    CHECK(!msg::event_for_screen(State::Ready, State::Review, &e));
    CHECK(!msg::event_for_screen(State::Cancelled, State::Review, &e));
    CHECK(!msg::event_for_screen(State::Executing, State::Review, &e));
}

HALO_TEST(messages_settings_fields_that_are_absent_keep_their_value) {
    msg::Settings st;
    const char* first =
        R"({"phone_ready":true,"orientation":"portrait","time":"2026-10-16T14:05:00-05:00",)"
        R"("tz":"America/Chicago"})";
    msg::SettingsUpdate u = msg::parse_settings(first, strlen(first), &st);
    CHECK(u.phone_ready && u.orientation && u.time && u.tz);
    CHECK(st.phone_ready && st.portrait);
    CHECK(std::string(st.tz) == "America/Chicago");

    // The phone sends readiness on its own when the speech model is reloading.
    const char* only_ready = R"({"phone_ready":false})";
    u = msg::parse_settings(only_ready, strlen(only_ready), &st);
    CHECK(u.phone_ready && !u.orientation && !u.tz);
    CHECK(!st.phone_ready);
    CHECK(st.portrait);                               // kept
    CHECK(std::string(st.tz) == "America/Chicago");   // kept
}

HALO_TEST(messages_an_unknown_orientation_is_landscape_not_a_third_state) {
    msg::Settings st;
    st.portrait = true;
    const char* doc = R"({"orientation":"sideways"})";
    msg::parse_settings(doc, strlen(doc), &st);
    CHECK(!st.portrait);
}

HALO_TEST(messages_screen_can_carry_the_pending_execution_key) {
    // PROPOSED, waiting on Carlos. The device has to report pending_key in
    // hello, dismiss and status_query, and nothing in the agreed contract ever
    // hands it one. Reading it is harmless while the phone does not send it.
    const char* doc =
        R"({"sid":9,"screen":"EXECUTING","pending_key":"01HQZX9K3M7N2P4R6T8V0W1Y3Z"})";
    msg::Screen s;
    CHECK(msg::parse_screen(doc, strlen(doc), &s));
    CHECK(s.has_pending_key);
    CHECK(std::string(s.pending_key) == "01HQZX9K3M7N2P4R6T8V0W1Y3Z");

    const char* without = R"({"sid":10,"screen":"REVIEW"})";
    CHECK(msg::parse_screen(without, strlen(without), &s));
    CHECK(!s.has_pending_key && s.pending_key[0] == '\0');
}

HALO_TEST(messages_a_status_update_does_not_wipe_the_screen_under_it) {
    // The status bar update is the 495 ms partial refresh path. If parsing it
    // dropped the stored body, the next redraw would blank a REVIEW screen the
    // user is in the middle of reading, and report a clean partial refresh while
    // doing it.
    msg::Screen s;
    const char* review =
        R"({"sid":42,"screen":"REVIEW","rev":3,"screen_lines":["To: victor@example.com",)"
        R"("Subject: Meeting moved"],"status_line":"Ready to confirm"})";
    CHECK(msg::parse_screen(review, strlen(review), &s));

    const char* status = R"({"sid":43,"status_line":"Searching the web"})";
    CHECK(msg::parse_screen(status, strlen(status), &s));
    CHECK(!s.has_body);                                      // this write had no body
    CHECK(s.sid == 43);
    CHECK(std::string(s.status_line) == "Searching the web");
    CHECK(s.has_state && s.state == State::Review);          // still the screen underneath
    CHECK(s.line_count == 2);
    CHECK(std::string(s.lines[0]) == "To: victor@example.com");
    CHECK(s.line_ptrs[1] == s.lines[1]);
}

// ------------------------------------------------- fragment reassembly
//
// Every JSON write from the phone carries the 2 byte prefix, even when it fits
// in one packet. Found by Carlos on 2026-10-16 by reading the message layer: the
// device was passing the raw write to the parser, prefix and all.

namespace {

// Splits a document the way the phone does: `n` fragments, 2 byte prefix on each.
std::vector<std::vector<uint8_t>> fragment(const std::string& doc, uint8_t id, size_t pieces) {
    std::vector<std::vector<uint8_t>> out;
    const size_t each = (doc.size() + pieces - 1) / pieces;
    for (size_t i = 0; i < pieces; ++i) {
        const size_t at = i * each;
        const size_t take = at + each > doc.size() ? doc.size() - at : each;
        std::vector<uint8_t> f{id, static_cast<uint8_t>((i + 1 == pieces ? 0x80 : 0) | i)};
        f.insert(f.end(), doc.begin() + at, doc.begin() + at + take);
        out.push_back(f);
    }
    return out;
}

}  // namespace

HALO_TEST(reassembly_puts_a_three_fragment_review_screen_back_together) {
    // The case that breaks in the field: a full REVIEW screen is about 700 bytes
    // and the phone sends it as three writes at MTU 256. Treated as three screens
    // it is garbage; treated as one it is what the user reads and approves.
    std::string doc = R"({"sid":42,"screen":"REVIEW","rev":3,"status_line":"Ready to confirm","screen_lines":[)";
    for (int i = 0; i < 12; ++i) {
        if (i) doc += ",";
        doc += "\"line ";
        doc += static_cast<char>('A' + i);
        doc += std::string(40, 'x');
        doc += "\"";
    }
    doc += "]}";
    CHECK(doc.size() > 600);  // genuinely needs more than one write

    msg::Reassembler r;
    const auto pieces = fragment(doc, 7, 3);
    CHECK(r.feed(pieces[0].data(), pieces[0].size()) == msg::Reassembler::Result::Incomplete);
    CHECK(r.feed(pieces[1].data(), pieces[1].size()) == msg::Reassembler::Result::Incomplete);
    CHECK(r.feed(pieces[2].data(), pieces[2].size()) == msg::Reassembler::Result::Complete);
    CHECK(r.length() == doc.size());
    CHECK(std::string(r.message(), r.length()) == doc);

    msg::Screen s;
    CHECK(msg::parse_screen(r.message(), r.length(), &s));
    CHECK(s.sid == 42 && s.state == State::Review && s.line_count == 12);
}

HALO_TEST(reassembly_handles_the_ordinary_one_fragment_write) {
    const std::string doc = R"({"phone_ready":true})";
    msg::Reassembler r;
    const auto pieces = fragment(doc, 3, 1);
    CHECK(r.feed(pieces[0].data(), pieces[0].size()) == msg::Reassembler::Result::Complete);
    CHECK(std::string(r.message(), r.length()) == doc);

    msg::Settings st;
    msg::parse_settings(r.message(), r.length(), &st);
    CHECK(st.phone_ready);
}

HALO_TEST(reassembly_drops_a_skipped_fragment_rather_than_guessing) {
    const std::string doc = R"({"sid":1,"screen":"REVIEW","screen_lines":["a","b"]})";
    msg::Reassembler r;
    const auto pieces = fragment(doc, 5, 3);
    CHECK(r.feed(pieces[0].data(), pieces[0].size()) == msg::Reassembler::Result::Incomplete);
    // fragment 1 never arrives
    CHECK(r.feed(pieces[2].data(), pieces[2].size()) == msg::Reassembler::Result::Dropped);
    CHECK(std::string(r.note()) == "fragment number skipped");
}

HALO_TEST(reassembly_drops_a_message_id_that_changes_half_way) {
    const std::string doc = R"({"sid":1,"screen":"REVIEW"})";
    msg::Reassembler r;
    auto a = fragment(doc, 5, 2);
    auto b = fragment(doc, 6, 2);
    CHECK(r.feed(a[0].data(), a[0].size()) == msg::Reassembler::Result::Incomplete);
    CHECK(r.feed(b[1].data(), b[1].size()) == msg::Reassembler::Result::Dropped);
    CHECK(std::string(r.note()) == "message ID changed part way through a message");
}

HALO_TEST(reassembly_reports_a_partial_message_that_was_abandoned) {
    // Fragment 0 of a new message always wins, but the half built one that it
    // threw away means a write was lost, and that must reach the log.
    const std::string doc = R"({"sid":2,"screen":"REVIEW"})";
    msg::Reassembler r;
    auto a = fragment(doc, 5, 2);
    auto b = fragment(doc, 6, 1);
    CHECK(r.feed(a[0].data(), a[0].size()) == msg::Reassembler::Result::Incomplete);
    CHECK(r.feed(b[0].data(), b[0].size()) == msg::Reassembler::Result::Complete);
    CHECK(std::string(r.note()) == "a new message started before the previous one finished");
    CHECK(std::string(r.message(), r.length()) == doc);
}

HALO_TEST(reassembly_refuses_a_stray_fragment_and_a_runt_write) {
    msg::Reassembler r;
    const uint8_t stray[] = {7, 0x82, '{', '}'};  // fragment 2, nothing in progress
    CHECK(r.feed(stray, sizeof(stray)) == msg::Reassembler::Result::Dropped);
    CHECK(std::string(r.note()) == "fragment arrived with no message in progress");

    const uint8_t runt[] = {7};
    CHECK(r.feed(runt, sizeof(runt)) == msg::Reassembler::Result::Dropped);
    CHECK(std::string(r.note()) == "write is shorter than the 2 byte fragment prefix");
}

HALO_TEST(reassembly_refuses_a_message_past_the_one_kilobyte_ceiling) {
    msg::Reassembler r;
    std::vector<uint8_t> f{9, 0x00};
    f.insert(f.end(), 600, 'x');
    CHECK(r.feed(f.data(), f.size()) == msg::Reassembler::Result::Incomplete);
    std::vector<uint8_t> g{9, 0x01};
    g.insert(g.end(), 600, 'x');
    CHECK(r.feed(g.data(), g.size()) == msg::Reassembler::Result::Dropped);
    CHECK(std::string(r.note()) == "message is larger than the 1 KB ceiling");
}

HALO_TEST(reassembly_recovers_on_the_next_whole_message_after_a_drop) {
    // A dropped write must not poison the link: the next good screen has to land.
    msg::Reassembler r;
    const uint8_t stray[] = {1, 0x83, 'x'};
    CHECK(r.feed(stray, sizeof(stray)) == msg::Reassembler::Result::Dropped);

    const std::string doc = R"({"sid":99,"screen":"SUCCESS"})";
    const auto pieces = fragment(doc, 2, 2);
    CHECK(r.feed(pieces[0].data(), pieces[0].size()) == msg::Reassembler::Result::Incomplete);
    CHECK(r.feed(pieces[1].data(), pieces[1].size()) == msg::Reassembler::Result::Complete);
    CHECK(std::string(r.message(), r.length()) == doc);
    CHECK(std::string(r.note()).empty());
}
