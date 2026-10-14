// Waveshare ESP32-S3 e-Paper 3.97 backend (the badge, the actual product).
//
// A logging stub on purpose: the board is not in hand. It exists and compiles
// from day one so that the swap is a build flag rather than a porting project,
// and so nothing above the display boundary can quietly grow a dependency on the
// development panel's size or colour depth.
#include "display_backends.hpp"
#include "esp_log.h"
#include "fonts/font_body.hpp"
#include "fonts/font_status.hpp"
#include "esp_timer.h"

namespace halo::board {
namespace {

constexpr const char* kTag = "panel";

class WaveshareDisplay : public Display {
public:
    bool init() override {
        ESP_LOGW(kTag, "Waveshare 3.97 backend is a STUB: logs what it would draw, no pixels");
        return true;
    }

    Caps caps() const override {
        Caps c;
        c.logical_w = 800;
        c.logical_h = 480;
        c.visible_h = 480;   // the badge shows the whole canvas
        // 1 bit black and white only (decided 2026-10-13, Victor). The panel can do
        // 4 gray levels, but 4 gray mode has no partial refresh at all on this
        // panel, so every grayscale screen would cost a full refresh.
        c.gray_levels = 2;
        c.supports_partial = true;
        return c;
    }

    // Spleen, the real font now compiled in. screen_lines are all one size so the
    // budget the backend publishes is a single number, which is what the contract
    // needs. Emphasis comes from inversion, not from a second size.
    TextMetrics metrics(TextStyle style) const override {
        if (style == TextStyle::Status) return TextMetrics{font::kStatusWidth, font::kStatusHeight};
        return TextMetrics{font::kBodyWidth, font::kBodyHeight};
    }

    void clear() override { ESP_LOGI(kTag, "[stub] clear"); }
    void draw(const DisplayList& list) override { ESP_LOGI(kTag, "[stub] draw %u commands", list.size()); }
    void present(Refresh intent, PresentedFn on_presented, void* ctx) override {
        ESP_LOGI(kTag, "[stub] present intent=%d", static_cast<int>(intent));
        if (on_presented) on_presented(ctx, static_cast<uint32_t>(esp_timer_get_time() / 1000));
    }
    void sleep() override { ESP_LOGI(kTag, "[stub] sleep"); }
};

WaveshareDisplay g_display;

}  // namespace

Display& display() { return g_display; }
const char* panel_name() { return "Waveshare 3.97 (STUB, board not in hand)"; }
void deep_clean(int cycles) { ESP_LOGI(kTag, "[stub] deep clean %d cycles", cycles); }
void debug_pattern(int which) { ESP_LOGI(kTag, "[stub] pattern %d", which); }

}  // namespace halo::board
