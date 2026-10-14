#include "halo/layout.hpp"

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
    if (m.status_line != nullptr && m.status_line[0] != '\0') {
        TextCmd t;
        t.box = Box{kMargin, 0, static_cast<int16_t>(c.logical_w - 2 * kMargin), kStatusH};
        t.text = m.status_line;
        t.style = TextStyle::Status;
        out.text(t);
        out.line(LineCmd{kMargin, kDividerY, static_cast<int16_t>(c.logical_w - kMargin), kDividerY, 0});
    }

    int16_t y = body.y;

    // An icon, either the whole screen or above the text.
    if (m.icon != nullptr && m.icon->bits != nullptr) {
        const bool alone = (m.line_count == 0 && m.candidate_count == 0);
        BitmapCmd b;
        b.bits = m.icon->bits;
        b.src_w = m.icon->w;
        b.src_h = m.icon->h;
        const int16_t h = alone ? body.h : static_cast<int16_t>(m.icon->h);
        b.box = Box{body.x, y, body.w, h};
        out.bitmap(b);
        if (alone) return 0;
        y = static_cast<int16_t>(y + h + 8);
    }

    const int16_t remaining_h = static_cast<int16_t>(body.bottom() - y);

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
        if (m.hint != nullptr) {
            TextCmd t;
            t.box = Box{body.x, static_cast<int16_t>(view_h(c) - kFooterH), body.w, kFooterH};
            t.text = m.hint;
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
        if (m.hint != nullptr) {
            TextCmd t;
            t.box = Box{body.x, static_cast<int16_t>(view_h(c) - kFooterH), body.w, kFooterH};
            t.text = m.hint;
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
