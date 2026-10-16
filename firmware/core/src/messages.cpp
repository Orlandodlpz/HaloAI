#include "halo/messages.hpp"

#include "halo/json.hpp"

namespace halo::msg {
namespace {

// A generous ceiling for one event. The largest is `hello`, at about 150 bytes
// with a 26 character pending key.
constexpr size_t kMinEventBuf = 32;

bool streq(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

// sid and pending_key are the two fields that are null rather than absent when
// the device has none. The phone distinguishes them, so neither may be dropped.
void put_sid(json::Writer& w, const Link& l) {
    if (l.sid < 0) w.key_null("sid");
    else w.key_int("sid", l.sid);
}

void put_pending_key(json::Writer& w, const Link& l) {
    if (l.pending_key[0] == '\0') w.key_null("pending_key");
    else w.key_str("pending_key", l.pending_key);
}

size_t done(json::Writer& w) { return w.finish() != nullptr ? w.length() : 0; }

// Every builder starts the same way, and a buffer too small to hold even an
// empty object would otherwise be written past.
bool too_small(size_t n) { return n < kMinEventBuf; }

}  // namespace

const char* name(CancelReason r) {
    switch (r) {
        case CancelReason::User: return "user";
        case CancelReason::Expired: return "expired";
        case CancelReason::Transport: return "transport";
        case CancelReason::Battery: return "battery";
    }
    return "user";
}

const char* wire_name(State s) {
    switch (s) {
        case State::BootRecovery: return "BOOT_RECOVERY";
        case State::Ready: return "READY";
        case State::Wallpaper: return "WALLPAPER";
        case State::CriticalBattery: return "CRITICAL_BATTERY";
        case State::PhoneCheck: return "PHONE_BLUETOOTH_CHECK";
        case State::PhoneDisconnected: return "PHONE_DISCONNECTED";
        case State::Recording: return "RECORDING";
        case State::FinalizeAudio: return "FINALIZE_AUDIO";
        case State::Processing: return "PROCESSING";
        case State::ProcessingSlow: return "PROCESSING_SLOW";
        case State::RecordAddition: return "RECORD_ADDITION";
        case State::ConflictAmbiguous: return "CONFLICT_AMBIGUOUS";
        case State::ConflictSchedule: return "CONFLICT_SCHEDULE";
        case State::Review: return "REVIEW";
        case State::ReadOnlyResult: return "READ_ONLY_RESULT";
        case State::Executing: return "EXECUTING";
        case State::ExecutionUncertain: return "EXECUTION_UNCERTAIN";
        case State::ReconcileStatus: return "RECONCILE_STATUS";
        case State::Success: return "SUCCESS";
        case State::Failure: return "FAILURE";
        case State::Cancelled: return "CANCELLED";
        default: return "?";
    }
}

bool state_from_wire(const char* text, State* out) {
    for (uint8_t i = 0; i < kStateCount; ++i) {
        const State s = static_cast<State>(i);
        if (streq(text, wire_name(s))) {
            *out = s;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ builders

size_t hello(char* out, size_t n, const Link& l, const char* fw, uint32_t boot, State state) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "hello");
    w.key_str("fw", fw);
    w.key_uint("boot", boot);
    w.key_str("state", wire_name(state));
    w.key_int("req", l.req);
    put_sid(w, l);
    put_pending_key(w, l);
    w.key_int("battery", l.battery);
    return done(w);
}

size_t wake(char* out, size_t n) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "wake");
    return done(w);
}

size_t record_start(char* out, size_t n, const Link& l, bool addition) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "record_start");
    w.key_int("req", l.req);
    w.key_int("seg", l.seg);
    w.key_int("rev", l.rev);
    w.key_bool("addition", addition);
    return done(w);
}

size_t record_end(char* out, size_t n, const Link& l, uint16_t chunks, uint32_t ms) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "record_end");
    w.key_int("req", l.req);
    w.key_int("seg", l.seg);
    w.key_int("chunks", chunks);
    w.key_uint("ms", ms);
    return done(w);
}

size_t approve(char* out, size_t n, const Link& l) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "approve");
    put_sid(w, l);
    return done(w);
}

size_t select(char* out, size_t n, const Link& l, int32_t index) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "select");
    put_sid(w, l);
    w.key_int("index", index);
    return done(w);
}

size_t continue_(char* out, size_t n, const Link& l) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "continue");
    put_sid(w, l);
    return done(w);
}

size_t cancel(char* out, size_t n, const Link& l, CancelReason reason) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "cancel");
    w.key_int("req", l.req);
    put_sid(w, l);
    w.key_str("reason", name(reason));
    return done(w);
}

size_t dismiss(char* out, size_t n, const Link& l) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "dismiss");
    put_sid(w, l);
    put_pending_key(w, l);
    return done(w);
}

size_t ack(char* out, size_t n, const Link& l) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "ack");
    put_sid(w, l);
    return done(w);
}

size_t status_query(char* out, size_t n, const Link& l) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "status_query");
    put_pending_key(w, l);
    return done(w);
}

size_t activity(char* out, size_t n, const Link& l) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "activity");
    put_sid(w, l);
    return done(w);
}

size_t battery(char* out, size_t n, uint8_t percent) {
    if (too_small(n)) return 0;
    json::Writer w(out, n);
    w.key_str("type", "battery");
    w.key_int("battery", percent);
    return done(w);
}

// ------------------------------------------------------------------ incoming

bool parse_audio_ack(const char* doc, size_t len, AudioAck* out) {
    AudioAck a;
    if (!json::get_int(doc, len, "upto", &a.upto)) return false;  // the one required field
    json::get_int(doc, len, "req", &a.req);
    json::get_int(doc, len, "seg", &a.seg);
    a.has_resend = json::get_int(doc, len, "resend_from", &a.resend_from);
    json::get_bool(doc, len, "final", &a.final_segment);
    *out = a;
    return true;
}

void Screen::clear() {
    sid = -1;
    state = State::Ready;
    has_state = has_rev = has_body = has_status = truncated = false;
    has_pending_key = false;
    pending_key[0] = '\0';
    rev = 0;
    line_count = candidate_count = 0;
    expires_in_s = 0;
    status_line[0] = '\0';
    for (uint8_t i = 0; i < kMaxLines; ++i) {
        lines[i][0] = '\0';
        line_ptrs[i] = nullptr;
    }
    for (uint8_t i = 0; i < kMaxCandidates; ++i) {
        candidates[i][0] = '\0';
        candidate_ptrs[i] = nullptr;
    }
}

bool parse_screen(const char* doc, size_t len, Screen* out) {
    int32_t sid = -1;
    if (!json::get_int(doc, len, "sid", &sid)) return false;

    char state_text[32] = {};
    State state = State::Ready;
    bool has_state = false;
    if (json::get_str(doc, len, "screen", state_text, sizeof(state_text))) {
        if (!state_from_wire(state_text, &state)) return false;  // unknown: drop the write
        has_state = true;
    }

    const size_t lines = json::array_len(doc, len, "screen_lines");
    const size_t cands = json::array_len(doc, len, "candidates");

    // A write with neither a state nor any body lines is a status bar update. It
    // must leave the body exactly as it is: that is the partial refresh path, and
    // clearing the stored lines here would blank the screen on the next redraw
    // while reporting a 495 ms partial refresh as if nothing had been lost.
    const bool status_only = !has_state && lines == 0;

    // Nothing is written into `out` until the whole message has been accepted, so
    // a rejected write can never leave half a screen behind.
    if (!status_only) {
        out->clear();
        out->state = state;
        out->has_state = has_state;
    }
    out->sid = sid;
    out->has_body = !status_only;
    out->has_rev = json::get_int(doc, len, "rev", &out->rev);
    out->has_status =
        json::get_str(doc, len, "status_line", out->status_line, sizeof(out->status_line));
    out->has_pending_key =
        json::get_str(doc, len, "pending_key", out->pending_key, sizeof(out->pending_key));
    out->truncated = lines > kMaxLines || cands > kMaxCandidates;

    const size_t take_lines = lines > kMaxLines ? kMaxLines : lines;
    for (size_t i = 0; i < take_lines; ++i) {
        if (!json::get_str_at(doc, len, "screen_lines", i, out->lines[i], kMaxLineChars + 1))
            break;
        out->line_ptrs[i] = out->lines[i];
        out->line_count = static_cast<uint8_t>(i + 1);
    }
    const size_t take_cands = cands > kMaxCandidates ? kMaxCandidates : cands;
    for (size_t i = 0; i < take_cands; ++i) {
        if (!json::get_str_at(doc, len, "candidates", i, out->candidates[i], kMaxLineChars + 1))
            break;
        out->candidate_ptrs[i] = out->candidates[i];
        out->candidate_count = static_cast<uint8_t>(i + 1);
    }

    int32_t expires = 0;
    if (json::get_int(doc, len, "expires_in_s", &expires) && expires > 0)
        out->expires_in_s = static_cast<uint16_t>(expires > 65535 ? 65535 : expires);
    return true;
}

bool event_for_screen(State screen, State current, EventKind* out) {
    switch (screen) {
        case State::Review: *out = EventKind::ScreenReview; return true;
        case State::ConflictAmbiguous: *out = EventKind::ScreenConflictAmbiguous; return true;
        case State::ConflictSchedule: *out = EventKind::ScreenConflictSchedule; return true;
        case State::ReadOnlyResult: *out = EventKind::ScreenReadOnly; return true;
        case State::ProcessingSlow: *out = EventKind::ProcessingSlow; return true;
        case State::Processing: *out = EventKind::ProcessingProgress; return true;
        case State::Success: *out = EventKind::ExecutionApplied; return true;
        case State::ExecutionUncertain: *out = EventKind::ExecutionUnknown; return true;
        case State::Failure:
            // The same screen name means two different things, and they differ in
            // whether a pending execution key may be cleared.
            *out = (current == State::Executing || current == State::ExecutionUncertain ||
                    current == State::ReconcileStatus)
                       ? EventKind::ExecutionRejected
                       : EventKind::ProcessingFailed;
            return true;
        default:
            return false;
    }
}

SettingsUpdate parse_settings(const char* doc, size_t len, Settings* inout) {
    SettingsUpdate u;
    u.phone_ready = json::get_bool(doc, len, "phone_ready", &inout->phone_ready);

    char orientation[16] = {};
    if (json::get_str(doc, len, "orientation", orientation, sizeof(orientation))) {
        // Anything that is not "portrait" is landscape: the contract has two
        // values, and a typo must not leave the panel in a third state.
        inout->portrait = streq(orientation, "portrait");
        u.orientation = true;
    }
    u.time = json::get_str(doc, len, "time", inout->time, sizeof(inout->time));
    u.tz = json::get_str(doc, len, "tz", inout->tz, sizeof(inout->tz));
    return u;
}

}  // namespace halo::msg
