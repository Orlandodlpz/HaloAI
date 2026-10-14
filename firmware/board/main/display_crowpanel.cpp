// CrowPanel ESP32 E-Paper 5.79 backend (development board).
//
// F0 scope: capabilities, metrics and logging. Real rasterising and the SSD1683
// command sequence are F1 and are NOT started. What is already correct here is
// the shape the rest of the firmware sees, so F1 fills in pixels without anything
// above the boundary changing.
//
// Panel facts that F1 needs, recorded in docs/CLAUDE.md: Good Display GDEY0579T93,
// framebuffer 800 x 272 (27200 bytes) at 1 bit per pixel, most significant bit
// leftmost, 1 means white. The two SSD1683 drivers own 400 columns each, of which
// 396 are visible, so 8 columns at the seam are dead and the driver skips them.
// Slave command = master command with bit 7 set. IO7 must be high first.
#include "board_pins.hpp"
#include "display_backends.hpp"
#include "esp_log.h"
#include "esp_timer.h"

namespace halo::board {
namespace {

constexpr const char* kTag = "panel";

class CrowPanelDisplay : public Display {
public:
    bool init() override {
        // F1: drive kPanelPower high, reset, init SPI, send the SSD1683 sequence.
        ESP_LOGI(kTag, "init: CrowPanel 5.79, 792x272 visible, 2 levels (F0 stub, no pixels yet)");
        return true;
    }

    Caps caps() const override {
        Caps c;
        c.logical_w = 800;   // the badge canvas everything is laid out against
        c.logical_h = 480;
        c.visible_h = 272;   // this panel shows 272 of those 480 units at a time
        c.gray_levels = 2;
        c.supports_partial = true;
        return c;
    }

    // PROVISIONAL until the font is chosen in F1. A 12 x 24 cell on a 792 px wide
    // panel gives 66 columns, which is within a character of what the 800 px badge
    // will give, so wrapping developed here should transfer almost unchanged.
    TextMetrics metrics(TextStyle style) const override {
        switch (style) {
            case TextStyle::Title: return TextMetrics{16, 32};
            case TextStyle::Status: return TextMetrics{10, 20};
            default: return TextMetrics{12, 24};
        }
    }

    void clear() override { ESP_LOGI(kTag, "clear"); }

    void draw(const DisplayList& list) override {
        ESP_LOGI(kTag, "draw: %u commands%s", list.size(),
                 list.dropped() ? " (OVERFLOWED, screen is too complex)" : "");
        for (uint8_t i = 0; i < list.size(); ++i) {
            const DrawCmd& c = list[i];
            if (c.kind == CmdKind::Text)
                ESP_LOGI(kTag, "  text  (%d,%d) \"%s\"%s", c.text.box.x, c.text.box.y,
                         c.text.text, c.text.invert ? " inverted" : "");
            else if (c.kind == CmdKind::Bitmap)
                ESP_LOGI(kTag, "  bitmap (%d,%d) %ux%u", c.bitmap.box.x, c.bitmap.box.y,
                         c.bitmap.src_w, c.bitmap.src_h);
            else if (c.kind == CmdKind::Rect)
                ESP_LOGI(kTag, "  rect  (%d,%d) %dx%d", c.rect.box.x, c.rect.box.y,
                         c.rect.box.w, c.rect.box.h);
            else
                ESP_LOGI(kTag, "  line  (%d,%d)-(%d,%d)", c.line.x0, c.line.y0, c.line.x1, c.line.y1);
        }
    }

    void present(Refresh intent, PresentedFn on_presented, void* ctx) override {
        static const char* kNames[] = {"partial", "fast", "full"};
        ESP_LOGI(kTag, "present: %s", kNames[static_cast<int>(intent)]);
        // F1 replaces this with the real wait on BUSY. The callback firing late is
        // the whole point: it is what starts the REVIEW confirm guard.
        if (on_presented) on_presented(ctx, static_cast<uint32_t>(esp_timer_get_time() / 1000));
    }

    void sleep() override { ESP_LOGI(kTag, "deep sleep (needs a hardware reset to wake)"); }
};

CrowPanelDisplay g_display;

}  // namespace

Display& display() { return g_display; }
const char* panel_name() { return "CrowPanel 5.79 (development board)"; }

}  // namespace halo::board
