// The BLE message layer: every JSON message in the agreed contract, built and
// read here rather than in the board glue.
//
// Contract: BRIEFING.md section 2 ("BLE JSON field names and UUIDs", agreed
// 2026-10-13 by Carlos and Victor), with the detail in
// phone/docs/ble_fields_draft.md and the device side notes in
// firmware/docs/ble_fields.md.
//
// It lives in core/ for the same reason the state machine does: this is the layer
// that decides what the phone is told the user approved, and it is testable on a
// computer against the exact bytes the phone expects. The board glue below it only
// moves bytes on and off the radio.
//
// Everything the phone sends is data, never instructions. Nothing here is
// evaluated, no length is trusted, and every copy is bounded.
#pragma once

#include <cstddef>
#include <cstdint>

#include "halo/events.hpp"
#include "halo/states.hpp"

namespace halo::msg {

// Straight from the published text budget: 48 characters by 12 lines landscape,
// 28 by 22 portrait (BRIEFING section 2). The device keeps room for the larger of
// each dimension, so one buffer serves both orientations.
constexpr uint8_t kMaxLines = 22;
constexpr uint8_t kMaxCandidates = 8;
constexpr size_t kMaxLineChars = 48;
constexpr size_t kPendingKeyChars = 26;  // the backend's short execution key

// Wire spelling of a state: the spec name with every run of characters that are
// not letters or digits replaced by one underscore. "CONFLICT-AMBIGUOUS" becomes
// CONFLICT_AMBIGUOUS and "PHONE / BLUETOOTH CHECK" becomes PHONE_BLUETOOTH_CHECK.
// A test pins the whole table against that rule, so the two spellings cannot
// drift apart.
const char* wire_name(State s);
bool state_from_wire(const char* text, State* out);

// Device local identity: everything an event echoes. Builders take this by
// reference rather than loose integers so a message cannot be given an ID that
// does not belong to the device's current request.
struct Link {
    uint16_t req = 0;   // device local request counter
    uint8_t seg = 0;    // device local segment counter
    uint8_t rev = 0;    // low 8 bits of the revision last displayed
    int32_t sid = -1;   // last screen drawn; -1 means none, and is sent as null
    uint8_t battery = 100;
    char pending_key[kPendingKeyChars + 1] = {};  // empty is sent as null
};

// Why the device cancelled. Without it the phone cannot tell "the user changed
// their mind" from "the device gave up", which the per-hop logs in I4 need and
// which decides whether anything should be retried.
enum class CancelReason : uint8_t { User, Expired, Transport, Battery };
const char* name(CancelReason r);

// Builders. Each writes one complete JSON object into `out` and returns its
// length, or 0 if it did not fit. 0 means do not send: a truncated object is not
// JSON, and half an approve is worse than no approve.
size_t hello(char* out, size_t n, const Link& l, const char* fw, uint32_t boot, State state);
size_t wake(char* out, size_t n);
size_t record_start(char* out, size_t n, const Link& l, bool addition);
size_t record_end(char* out, size_t n, const Link& l, uint16_t chunks, uint32_t ms);
size_t approve(char* out, size_t n, const Link& l);
size_t select(char* out, size_t n, const Link& l, int32_t index);
size_t continue_(char* out, size_t n, const Link& l);
size_t cancel(char* out, size_t n, const Link& l, CancelReason reason);
size_t dismiss(char* out, size_t n, const Link& l);
size_t ack(char* out, size_t n, const Link& l);
size_t status_query(char* out, size_t n, const Link& l);
size_t activity(char* out, size_t n, const Link& l);
size_t battery(char* out, size_t n, uint8_t percent);

// ------------------------------------------------------------ from the phone

struct AudioAck {
    int32_t req = 0;
    int32_t seg = 0;
    int32_t upto = -1;  // -1 until chunk 0 arrives
    int32_t resend_from = -1;
    bool has_resend = false;
    bool final_segment = false;
};
bool parse_audio_ack(const char* doc, size_t len, AudioAck* out);

// One Screen write, copied out of the receive buffer.
//
// Holds pointers into itself so it can be handed to ScreenModel without another
// copy, which is why it cannot be copied: copying would leave the pointers
// aimed at the old object. Keep one and parse into it.
struct Screen {
    Screen() = default;
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    // Back to empty, including the self pointers. parse_screen calls this only
    // once the whole write has been accepted.
    void clear();

    int32_t sid = -1;
    State state = State::Ready;
    bool has_state = false;
    int32_t rev = 0;
    bool has_rev = false;
    // False when the write carried only a status_line. That is the partial
    // refresh path: the status bar changes and the body is left alone.
    bool has_body = false;
    bool has_status = false;
    uint8_t line_count = 0;
    uint8_t candidate_count = 0;
    uint16_t expires_in_s = 0;
    // True when the phone sent more lines or candidates than the budget allows.
    // Reported rather than hidden: it means the backend and the device disagree
    // about the text budget, which is a contract bug, not a display detail.
    bool truncated = false;
    // PROPOSED, not in the agreed contract (see firmware/docs/ble_fields.md,
    // answer 5). The device is asked to report `pending_key` in `hello`,
    // `dismiss` and `status_query`, but nothing in the contract ever gives it
    // one. Carried on the EXECUTING screen is the obvious place: that is the
    // moment the backend has an execution to report on. Absent until Carlos
    // signs it off, which is harmless: the field is simply never set.
    bool has_pending_key = false;
    char pending_key[kPendingKeyChars + 1] = {};

    char status_line[kMaxLineChars + 1] = {};
    char lines[kMaxLines][kMaxLineChars + 1] = {};
    char candidates[kMaxCandidates][kMaxLineChars + 1] = {};
    const char* line_ptrs[kMaxLines] = {};
    const char* candidate_ptrs[kMaxCandidates] = {};
};

// False if the write is not a usable screen (no sid, or a `screen` the device
// does not know). A screen that fails to parse is dropped, never half applied.
bool parse_screen(const char* doc, size_t len, Screen* out);

// The state machine event a screen implies, if any. `current` matters for one
// case: a FAILURE screen means the execution was rejected if the device was
// executing, and that processing failed otherwise. The two differ in whether a
// pending execution key may be cleared, so they cannot be collapsed.
//
// READY, CANCELLED, EXECUTING and the device's own local states imply nothing:
// either the device is already there or the V1 table has no event for it.
bool event_for_screen(State screen, State current, EventKind* out);

struct Settings {
    bool phone_ready = false;
    bool portrait = false;
    char time[40] = {};
    char tz[48] = {};
};

// What one Settings write changed. Absent fields keep their previous value, so
// this merges into `inout` and reports which fields were actually present.
struct SettingsUpdate {
    bool phone_ready = false;
    bool orientation = false;
    bool time = false;
    bool tz = false;
};
SettingsUpdate parse_settings(const char* doc, size_t len, Settings* inout);

}  // namespace halo::msg
