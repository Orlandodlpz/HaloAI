// Waveshare ESP32-S3 e-Paper 3.97 backend (the badge, the actual product).
//
// A logging stub on purpose: the board is not in hand. It exists and compiles
// from day one so that the swap is a build flag rather than a porting project,
// and so nothing above the display boundary can quietly grow a dependency on the
// development panel's size or colour depth.
#include "display_backends.hpp"
#include "esp_log.h"
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
        c.gray_levels = 4;
        c.supports_partial = true;
        return c;
    }

    // PROVISIONAL until the font is chosen in F1 and measured on the real panel.
    TextMetrics metrics(TextStyle style) const override {
        switch (style) {
            case TextStyle::Title: return TextMetrics{16, 32};
            case TextStyle::Status: return TextMetrics{10, 20};
            default: return TextMetrics{12, 24};
        }
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

}  // namespace halo::board
