#include "halo/layout.hpp"

#include "halo/screens.hpp"

namespace halo {

namespace {

// Logical units, on the 800 x 480 badge canvas.
constexpr int16_t kMargin = 16;
constexpr int16_t kStatusH = 40;
constexpr int16_t kDividerY = 44;
constexpr int16_t kBodyTop = 56;
constexpr int16_t kFooterH = 36;

// The window the user can actually see, which on the development panel is a lot
// shorter than the logical canvas. Laying out against logical_h would place lines
// below the glass where nothing can scroll to them.
int16_t view_h(const Caps& c) {
    return c.visible_h < c.logical_h ? c.visible_h : c.logical_h;
}

// Writes an unsigned number and returns how many characters it took. stdio is
// not worth pulling in for two numbers on a status bar.
uint8_t write_uint(char* out, size_t cap, uint16_t v) {
    char digits[6];
    uint8_t d = 0;
    do { digits[d++] = static_cast<char>('0' + (v % 10)); v = static_cast<uint16_t>(v / 10); }
    while (v != 0 && d < sizeof(digits));
    uint8_t n = 0;
    while (d-- > 0 && n + 1 < cap) out[n++] = digits[d];
    return n;
}

Box body_box(const Caps& c) {
    return Box{kMargin, kBodyTop,
               static_cast<int16_t>(c.logical_w - 2 * kMargin),
               static_cast<int16_t>(view_h(c) - kBodyTop - kFooterH)};
}

}  // namespace

// ---------------------------------------------------------------- display list

DrawCmd* DisplayList::next() {
    if (count_ >= kMax) {
        if (dropped_ < 255) ++dropped_;
        return nullptr;
    }
    return &items_[count_++];
}

void DisplayList::text(const TextCmd& c) {
    if (DrawCmd* d = next()) {
        d->kind = CmdKind::Text;
        d->text = c;
    }
}

void DisplayList::rect(const RectCmd& c) {
    if (DrawCmd* d = next()) {
        d->kind = CmdKind::Rect;
        d->rect = c;
    }
}

void DisplayList::line(const LineCmd& c) {
    if (DrawCmd* d = next()) {
        d->kind = CmdKind::Line;
        d->line = c;
    }
}

void DisplayList::bitmap(const BitmapCmd& c) {
    if (DrawCmd* d = next()) {
        d->kind = CmdKind::Bitmap;
        d->bitmap = c;
    }
}

// ---------------------------------------------------------------- helpers

uint8_t lines_that_fit(const Display& d, TextStyle style, int16_t box_h) {
    const TextMetrics m = d.metrics(style);
    if (m.line_h == 0 || box_h <= 0) return 0;
    return static_cast<uint8_t>(box_h / m.line_h);
}

uint8_t chars_that_fit(const Display& d, TextStyle style, int16_t box_w) {
    const TextMetrics m = d.metrics(style);
    if (m.char_w == 0 || box_w <= 0) return 0;
    const int16_t n = box_w / m.char_w;
    return static_cast<uint8_t>(n > 255 ? 255 : n);
}

// ---------------------------------------------------------------- layout

uint8_t layout_screen(const Display& d, const ScreenModel& m, DisplayList& out) {
    out.clear();
    const Caps c = d.caps();
    const Box body = body_box(c);

    // Status bar. Its own region so a status-only change can be a partial refresh.
    // The right hand end of the status bar: how long is left to decide, and how
    // much battery there is. Both were in ScreenModel and neither was ever
    // drawn; `expires_in_s` even said "shown in the status bar" in its own
    // comment. Same family as the scroll that never redrew.
    //
    // Built right to left into one small buffer and placed by hand, because
    // TextCmd::align is advisory and neither display backend honours it: they
    // both start at box.x.
    static char right[16];
    uint8_t rn = 0;
    if (m.battery_pct >= 0) {
        if (m.charging && rn + 1 < sizeof(right)) right[rn++] = '+';
        rn = static_cast<uint8_t>(rn + write_uint(right + rn, sizeof(right) - rn,
                                                  static_cast<uint16_t>(m.battery_pct)));
        if (rn + 1 < sizeof(right)) right[rn++] = '%';
    }
    if (m.expires_in_s > 0) {
        if (rn > 0 && rn + 2 < sizeof(right)) { right[rn++] = ' '; right[rn++] = ' '; }
        rn = static_cast<uint8_t>(rn + write_uint(right + rn, sizeof(right) - rn, m.expires_in_s));
        if (rn + 1 < sizeof(right)) right[rn++] = 's';
    }
    right[rn] = '\0';

    const TextMetrics sm = d.metrics(TextStyle::Status);
    const int16_t right_w = static_cast<int16_t>(rn * sm.char_w);

    if (m.status_line != nullptr && m.status_line[0] != '\0') {
        TextCmd t;
        // Stops short of the right hand block, so a long status line is clipped
        // by its own box rather than running into the battery.
        t.box = Box{kMargin, 0,
                    static_cast<int16_t>(c.logical_w - 2 * kMargin - right_w), kStatusH};
        t.text = m.status_line;
        t.style = TextStyle::Status;
        out.text(t);
        out.line(LineCmd{kMargin, kDividerY, static_cast<int16_t>(c.logical_w - kMargin), kDividerY, 0});
    }
    if (rn > 0) {
        TextCmd t;
        t.box = Box{static_cast<int16_t>(c.logical_w - kMargin - right_w), 0, right_w, kStatusH};
        t.text = right;
        t.style = TextStyle::Status;
        t.align = Align::Right;
        out.text(t);
    }

    int16_t y = body.y;

    // A full canvas image is a wallpaper: it covers the glass, status bar and all,
    // and the backend fits it to however much of the canvas that panel shows. Any
    // smaller icon is placed in the body like a picture above the text.
    if (m.icon != nullptr && m.icon->bits != nullptr && m.icon->w == c.logical_w &&
        m.icon->h == c.logical_h) {
        BitmapCmd b;
        b.bits = m.icon->bits;
        b.src_w = m.icon->w;
        b.src_h = m.icon->h;
        b.invert = m.icon->invert;
        b.box = Box{0, 0, c.logical_w, c.visible_h};
        b.scale_to_box = true;
        out.clear();   // nothing shares the glass with a wallpaper
        out.bitmap(b);
        return 0;
    }

    // An icon, either the whole screen or above the text.
    if (m.icon != nullptr && m.icon->bits != nullptr) {
        const bool alone = (m.line_count == 0 && m.candidate_count == 0);
        BitmapCmd b;
        b.bits = m.icon->bits;
        b.invert = m.icon->invert;
        b.src_w = m.icon->w;
        b.src_h = m.icon->h;
        const int16_t h = alone ? body.h : static_cast<int16_t>(m.icon->h);
        b.box = Box{body.x, y, body.w, h};
        out.bitmap(b);
        if (alone) return 0;
        y = static_cast<int16_t>(y + h + 8);
    }

    const int16_t remaining_h = static_cast<int16_t>(body.bottom() - y);

    // No candidates at all on CONFLICT-AMBIGUOUS: the backend found nothing
    // matching the name that was said. Its lines say so; the way out is a
    // gesture, so the device names it. Only when the screen did not bring its
    // own hint, so this can never overwrite something deliberate.
    const char* hint = m.hint;
    if (m.state == State::ConflictAmbiguous && m.candidate_count == 0 && hint == nullptr)
        hint = no_candidates_hint();

    // Candidates: one row each, the highlighted one drawn inverted.
    if (m.candidate_count > 0 && m.candidates != nullptr) {
        const TextMetrics tm = d.metrics(TextStyle::Body);
        const uint8_t room = lines_that_fit(d, TextStyle::Body, remaining_h);
        const uint8_t shown = m.candidate_count < room ? m.candidate_count : room;
        for (uint8_t i = 0; i < shown; ++i) {
            const Box row{body.x, static_cast<int16_t>(y + i * tm.line_h), body.w, tm.line_h};
            const bool on = (m.highlight >= 0 && static_cast<uint8_t>(m.highlight) == i);
            if (on) out.rect(RectCmd{row, true, 0});
            TextCmd t;
            t.box = row;
            t.text = m.candidates[i];
            t.style = TextStyle::Body;
            t.invert = on;
            out.text(t);
        }
        if (hint != nullptr) {
            TextCmd t;
            t.box = Box{body.x, static_cast<int16_t>(view_h(c) - kFooterH), body.w, kFooterH};
            t.text = hint;
            t.style = TextStyle::Status;
            out.text(t);
        }
        return static_cast<uint8_t>(m.candidate_count - shown);
    }

    // Body lines, already wrapped by the backend. Never re-wrapped here: what the
    // backend checked against the text budget is exactly what gets drawn.
    if (m.line_count > 0 && m.lines != nullptr) {
        const TextMetrics tm = d.metrics(TextStyle::Body);
        const uint8_t room = lines_that_fit(d, TextStyle::Body, remaining_h);
        const uint8_t first = m.scroll_line < m.line_count ? m.scroll_line : m.line_count;
        const uint8_t left = static_cast<uint8_t>(m.line_count - first);
        const uint8_t shown = left < room ? left : room;
        for (uint8_t i = 0; i < shown; ++i) {
            TextCmd t;
            t.box = Box{body.x, static_cast<int16_t>(y + i * tm.line_h), body.w, tm.line_h};
            t.text = m.lines[first + i];
            t.style = (i == 0 && m.state == State::Review) ? TextStyle::Title : TextStyle::Body;
            out.text(t);
        }
        if (hint != nullptr) {
            TextCmd t;
            t.box = Box{body.x, static_cast<int16_t>(view_h(c) - kFooterH), body.w, kFooterH};
            t.text = hint;
            t.style = TextStyle::Status;
            out.text(t);
        }
        return static_cast<uint8_t>(left - shown);
    }

    return 0;
}

// ---------------------------------------------------------------- refresh policy

Refresh RefreshPolicy::choose(State to, bool status_only) {
    // Wake and wallpaper are the two moments where a slow, clean redraw is free:
    // nobody is waiting on a confirm, so spend it clearing accumulated ghosting.
    if (to == State::Wallpaper || to == State::BootRecovery) return Refresh::Full;
    if (status_only) return Refresh::Partial;

    // NEVER while audio is in flight.
    //
    // A full refresh is 2.5 s on the badge and nearly 8 s when it clears
    // ghosting first, and the main loop does not run while the panel is being
    // driven, so a full refresh chosen in one of these states stalls the audio
    // stream behind it. On 2026-10-28 the every-tenth-screen full refresh
    // landed on FINALIZE AUDIO: the final marker went out 7.6 s after the
    // release instead of 0.65 s, which is almost the whole 8 s delivery
    // deadline, and the request failed. Found by Carlos on the real phone.
    //
    // PROCESSING and PROCESSING SLOW are here for the same reason even though
    // nothing is streaming: the user is waiting, the backend's answer is in
    // flight, and the main loop does not drain the inbox while the panel is
    // being driven, so the result screen arrives and sits in the queue. Carlos
    // measured it on the badge 2026-10-28: 5 of 7 commands reached the glass in
    // 2.9 to 3.2 s and the other 2 took 8.4 s, every time because the full
    // refresh landed on PROCESSING.
    //
    // Not every state the device is busy in. REVIEW and the conflict states are
    // busy too, but nobody is waiting on a round trip: a slow refresh there only
    // means the confirm guard starts later, which is exactly what it is for.
    //
    // The counter keeps running, so the ghost clearing is deferred to the next
    // screen that can afford it, not skipped.
    if (to == State::Recording || to == State::RecordAddition ||
        to == State::FinalizeAudio || to == State::Processing ||
        to == State::ProcessingSlow)
        return Refresh::Partial;

    if (full_every_ != 0 && partials_since_full_ >= full_every_) return Refresh::Full;
    return Refresh::Partial;
}

void RefreshPolicy::note_presented(Refresh used) {
    if (used == Refresh::Full) {
        partials_since_full_ = 0;
    } else if (partials_since_full_ < 255) {
        ++partials_since_full_;
    }
}

}  // namespace halo
