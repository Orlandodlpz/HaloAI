// The display boundary (option B, decided 2026-10-13 by Victor).
//
// Above this line: layout in LOGICAL units against a declared target size, emitted
// as a display list of text runs, rectangles and bitmaps. Nothing above the line
// knows the panel, its resolution, its controller or its vendor library.
//
// Below this line, in each backend: fonts and glyph rendering, mapping logical
// units onto real pixels, the refresh waveform, and panel quirks such as the
// CrowPanel's two SSD1683 drivers each covering half the panel.
//
// The logical canvas is the BADGE's panel (800 x 480 landscape), not the
// development board's. Layout is designed for the product and each backend crops
// or scales to its own panel, never the other way round, so we do not end up
// designing to the smaller screen and wasting the badge.
#pragma once

#include <cstddef>
#include <cstdint>

namespace halo {

// ---------------------------------------------------------------- geometry

struct Box {
    int16_t x = 0, y = 0, w = 0, h = 0;  // logical units
    int16_t right() const { return static_cast<int16_t>(x + w); }
    int16_t bottom() const { return static_cast<int16_t>(y + h); }
};

enum class Align : uint8_t { Left, Center, Right };

// Roles, not fonts. The backend picks a real font for each role, because only the
// backend knows how many pixels it has.
enum class TextStyle : uint8_t { Title, Body, Status, Mono, Count_ };

// Every present carries its intent. A fast LCD would ignore it; e-paper does not,
// and on the badge the difference is 0.6 s against 3.5 s.
enum class Refresh : uint8_t {
    Partial,  // small region, fastest, ghosts over time
    Fast,     // whole screen, quicker waveform, some ghosting
    Full,     // whole screen, slowest, clears ghosting
};

// ---------------------------------------------------------------- display list

enum class CmdKind : uint8_t { Text, Rect, Line, Bitmap };

struct TextCmd {
    Box box;
    const char* text = "";   // not owned; must outlive the draw call
    TextStyle style = TextStyle::Body;
    Align align = Align::Left;
    bool invert = false;     // highlighted candidate: light on dark
};

struct RectCmd {
    Box box;
    bool filled = false;
    uint8_t gray = 0;  // 0 = black, 255 = white; backends quantise to their levels
};

struct LineCmd {
    int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint8_t gray = 0;
};

// 1 bit per pixel, rows padded to whole bytes, most significant bit leftmost.
// A set bit is ink. This is the XBM-style layout icons are usually exported in.
struct BitmapCmd {
    Box box;                      // where it goes, in logical units
    const uint8_t* bits = nullptr;
    uint16_t src_w = 0, src_h = 0;
    bool scale_to_box = false;    // false: centred at native size
    bool invert = false;          // the source has 1 = white, not 1 = ink
};

struct DrawCmd {
    CmdKind kind = CmdKind::Text;
    union {
        TextCmd text;
        RectCmd rect;
        LineCmd line;
        BitmapCmd bitmap;
    };
    DrawCmd() : kind(CmdKind::Text), text() {}
};

// Fixed capacity: no allocation on the device. A V1 screen is a status bar, a
// handful of text lines and at most one bitmap, so this is generous.
class DisplayList {
public:
    static constexpr uint8_t kMax = 32;

    void clear() { count_ = 0; dropped_ = 0; }
    uint8_t size() const { return count_; }
    const DrawCmd& operator[](uint8_t i) const { return items_[i]; }
    // Non zero if a screen tried to draw more than fits: a bug worth logging.
    uint8_t dropped() const { return dropped_; }

    void text(const TextCmd& c);
    void rect(const RectCmd& c);
    void line(const LineCmd& c);
    void bitmap(const BitmapCmd& c);

private:
    DrawCmd* next();
    DrawCmd items_[kMax]{};
    uint8_t count_ = 0;
    uint8_t dropped_ = 0;
};

// ---------------------------------------------------------------- capabilities

// What the layout above the line is allowed to know about the panel: its logical
// size, and how big text is in logical units. Everything else stays in the backend.
struct TextMetrics {
    uint8_t char_w = 0;   // logical units per character (fixed width font)
    uint8_t line_h = 0;   // logical units per line, including leading
};

struct Caps {
    int16_t logical_w = 800;   // the canvas layout draws on
    int16_t logical_h = 480;
    // How much of the canvas this panel can actually show at once. The badge
    // shows all 480; the 792 x 272 development panel shows 272, so layout fits
    // fewer body lines there and the rest is reached by scrolling a line at a
    // time. Layout places against this, never against logical_h.
    int16_t visible_h = 480;
    uint8_t gray_levels = 2;
    bool supports_partial = true;
};

// ---------------------------------------------------------------- the interface

// present() is asynchronous on purpose. on_presented fires when the pixels are
// actually on the panel, which on e-paper is hundreds of ms after the call. That
// callback is what starts the REVIEW confirm guard: see state_machine.hpp.
using PresentedFn = void (*)(void* ctx, uint32_t at_ms);

class Display {
public:
    virtual ~Display() = default;

    virtual bool init() = 0;
    virtual Caps caps() const = 0;
    virtual TextMetrics metrics(TextStyle style) const = 0;

    virtual void clear() = 0;
    virtual void draw(const DisplayList& list) = 0;
    virtual void present(Refresh intent, PresentedFn on_presented, void* ctx) = 0;
    virtual void sleep() = 0;
};

// How many whole lines of this style fit in a box. Layout uses this instead of
// assuming a font size, which is what keeps the same screen code working on a
// 792 x 272 development panel and an 800 x 480 badge panel.
uint8_t lines_that_fit(const Display& d, TextStyle style, int16_t box_h);
uint8_t chars_that_fit(const Display& d, TextStyle style, int16_t box_w);

}  // namespace halo
