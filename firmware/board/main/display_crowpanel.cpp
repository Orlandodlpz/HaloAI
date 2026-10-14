// CrowPanel ESP32 E-Paper 5.79 backend: Good Display GDEY0579T93, two SSD1683
// drivers over SPI, 792 x 272 visible.
//
// The register sequence, the data entry modes and the master/slave split are
// taken from Elecrow's own factory source
// (Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792,
// factory_sourecode/5.79_tow/main/EPD_Init.cpp), because an e-paper init
// sequence is not something to reconstruct from a family datasheet. What is ours
// is the framebuffer, the rasteriser and the mapping from the logical canvas.
//
// Three things about this panel that are easy to get wrong:
//   - IO7 must be HIGH or the panel is dead. It gates the panel's ground return
//     through a MOSFET and is off at reset.
//   - The framebuffer is 800 wide even though only 792 columns are visible. Each
//     SSD1683 owns 400 columns of which 396 are shown, so there are 8 dead
//     columns at the seam that every pixel past 396 has to skip.
//   - A set bit is WHITE, not ink. Drawing black means clearing bits.
#include <cstring>

#include "board_pins.hpp"
#include "display_backends.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts/font_body.hpp"
#include "fonts/font_status.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace halo::board {
namespace {

constexpr const char* kTag = "panel";

constexpr int kBufW = 800;          // the framebuffer is 800 wide
constexpr int kVisibleW = 792;      // only 792 columns reach the glass
constexpr int kSeamAt = 396;        // pixels at or past this skip 8 dead columns
constexpr int kSeamSkip = 8;
constexpr int kHeight = 272;
constexpr int kStride = kBufW / 8;          // 100 bytes per row
constexpr int kHalfBytes = 400 / 8;         // 50 bytes per row per driver
constexpr size_t kFbBytes = kStride * kHeight;        // 27200
constexpr size_t kHalfScreen = kHalfBytes * kHeight;  // 13600 per driver

// 1 = white, 0 = black, so a cleared screen is all ones.
uint8_t g_fb[kFbBytes];
// What the controller believes is on the glass. The full refresh waveform reads
// it, and a present that writes only the new image RAM does nothing at all: that
// is why a partial refresh showed a screen and a full one of the same screen did
// not. Measured on the panel, 2026-10-13.
uint8_t g_fb_prev[kFbBytes];
uint8_t g_scratch[kHalfScreen];
spi_device_handle_t g_spi = nullptr;
bool g_ready = false;
bool g_baseline_done = false;

// ---------------------------------------------------------------- transport

void wait_busy() {
    // BUSY high means busy: Elecrow's loop waits for it to read 0.
    const int64_t deadline = esp_timer_get_time() + 20 * 1000 * 1000;
    while (gpio_get_level(kPanelBusy) != 0) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(kTag, "BUSY stuck high for 20 s: panel not responding "
                           "(is IO7 high?)");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void spi_send(const uint8_t* data, size_t len) {
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(g_spi, &t));
}

void cmd(uint8_t c) {
    gpio_set_level(kPanelDc, 0);
    spi_send(&c, 1);
    gpio_set_level(kPanelDc, 1);
}

void dat(uint8_t d) { spi_send(&d, 1); }

void dat_block(const uint8_t* p, size_t n) { spi_send(p, n); }

// ---------------------------------------------------------------- init

void hw_reset() {
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(kPanelRes, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(kPanelRes, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_busy();
}

void panel_init() {
    hw_reset();
    wait_busy();
    cmd(0x12);  // SWRESET
    wait_busy();
}

// Master driver: X increments, Y decrements from 271. Window and address counter
// are set separately because a second RAM write needs the counter reset without
// redefining the window.
void set_ram_master_window() {
    cmd(0x11); dat(0x05);
    cmd(0x44); dat(0x00); dat(0x31);
    cmd(0x45); dat(0x0f); dat(0x01); dat(0x00); dat(0x00);
}
void set_ram_master_addr() {
    cmd(0x4e); dat(0x00);
    cmd(0x4f); dat(0x0f); dat(0x01);
}

// Slave driver: the same registers with bit 7 set, and X decrements, because the
// second driver sits rotated with respect to the first.
void set_ram_slave_window() {
    cmd(0x91); dat(0x04);
    cmd(0xc4); dat(0x31); dat(0x00);
    cmd(0xc5); dat(0x0f); dat(0x01); dat(0x00); dat(0x00);
}
void set_ram_slave_addr() {
    cmd(0xce); dat(0x31);
    cmd(0xcf); dat(0x0f); dat(0x01);
}

void update(uint8_t mode);  // defined below

void fill_ram(uint8_t reg, uint8_t value) {
    cmd(reg);
    memset(g_scratch, value, kHalfScreen);
    dat_block(g_scratch, kHalfScreen);
}

// The panel wants each driver's half column by column, while the framebuffer is
// row major. Transpose into the scratch buffer so the whole half goes out in one
// SPI transaction instead of 13600 of them.
void transpose_half(const uint8_t* src, int byte_col_start) {
    size_t out = 0;
    for (int col = 0; col < kHalfBytes; ++col)
        for (int row = 0; row < kHeight; ++row)
            g_scratch[out++] = src[row * kStride + byte_col_start + col];
}

// Both RAMs, every time. 0x24 and 0xA4 take the new image, 0x26 and 0xA6 take
// what is currently on the glass. Writing only the new image leaves the
// controller comparing against whatever happened to be there, which on a full
// refresh means it decides nothing needs to move.
void push_framebuffer() {
    set_ram_master_window();
    set_ram_master_addr();
    cmd(0x24);
    transpose_half(g_fb, 0);
    dat_block(g_scratch, kHalfScreen);
    set_ram_master_addr();
    cmd(0x26);
    transpose_half(g_fb_prev, 0);
    dat_block(g_scratch, kHalfScreen);

    set_ram_slave_window();
    set_ram_slave_addr();
    cmd(0xa4);
    transpose_half(g_fb, kHalfBytes);
    dat_block(g_scratch, kHalfScreen);
    set_ram_slave_addr();
    cmd(0xa6);
    transpose_half(g_fb_prev, kHalfBytes);
    dat_block(g_scratch, kHalfScreen);

    memcpy(g_fb_prev, g_fb, sizeof(g_fb_prev));
}

// Drive every pixel hard between the two extremes a few times.
//
// Both RAMs get written, not just the new image one: 0x26 and 0xA6 hold what the
// controller believes is currently on the glass, and if that is stale (for
// example the vendor's factory image, still there after we flashed over it) the
// waveform computes the wrong transition and the old picture shows through. This
// is the one place that needs it, so ordinary presents stay cheap.
void clear_panel(int cycles) {
    for (int i = 0; i < cycles; ++i) {
        const uint8_t now = (i % 2) ? 0x00 : 0xFF;   // alternate black and white
        const uint8_t was = static_cast<uint8_t>(~now);
        set_ram_master_window();
        set_ram_master_addr();
        fill_ram(0x24, now);
        set_ram_master_addr();
        fill_ram(0x26, was);
        set_ram_slave_window();
        set_ram_slave_addr();
        fill_ram(0xa4, now);
        set_ram_slave_addr();
        fill_ram(0xa6, was);
        update(0xF7);
        memset(g_fb_prev, now, sizeof(g_fb_prev));
    }
    g_baseline_done = true;
}

void update(uint8_t mode) {
    cmd(0x22);
    dat(mode);
    cmd(0x20);
    wait_busy();
}

// ---------------------------------------------------------------- raster

inline void set_pixel(int x, int y, bool ink) {
    if (x < 0 || x >= kVisibleW || y < 0 || y >= kHeight) return;
    const int bx = (x >= kSeamAt) ? x + kSeamSkip : x;  // step over the dead seam
    uint8_t& byte = g_fb[y * kStride + (bx >> 3)];
    const uint8_t bit = static_cast<uint8_t>(0x80 >> (bx & 7));
    if (ink) byte = static_cast<uint8_t>(byte & ~bit);  // 0 is black
    else byte = static_cast<uint8_t>(byte | bit);
}

void fill_rect(int x, int y, int w, int h, bool ink) {
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) set_pixel(xx, yy, ink);
}

struct Font {
    const uint8_t* glyphs;
    uint8_t w, h, first, last;
    uint16_t stride, bytes;
};

Font font_for(TextStyle style) {
    if (style == TextStyle::Status)
        return Font{&font::kStatusGlyphs[0][0], font::kStatusWidth, font::kStatusHeight,
                    font::kStatusFirst, font::kStatusLast, font::kStatusStride,
                    static_cast<uint16_t>(font::kStatusStride * font::kStatusHeight)};
    return Font{&font::kBodyGlyphs[0][0], font::kBodyWidth, font::kBodyHeight, font::kBodyFirst,
                font::kBodyLast, font::kBodyStride,
                static_cast<uint16_t>(font::kBodyStride * font::kBodyHeight)};
}

void draw_text(const TextCmd& c) {
    const Font f = font_for(c.style);
    int x = c.box.x;
    for (const char* p = c.text; *p && x + f.w <= c.box.x + c.box.w; ++p, x += f.w) {
        const uint8_t ch = static_cast<uint8_t>(*p);
        if (ch < f.first || ch > f.last) continue;
        const uint8_t* g = f.glyphs + static_cast<size_t>(ch - f.first) * f.bytes;
        for (int row = 0; row < f.h; ++row)
            for (int bit = 0; bit < f.w; ++bit) {
                const uint8_t byte = g[row * f.stride + (bit >> 3)];
                const bool on = (byte >> (7 - (bit & 7))) & 1;
                // invert draws light text on a dark bar, for the highlighted
                // candidate in CONFLICT-AMBIGUOUS.
                set_pixel(x + bit, c.box.y + row, c.invert ? !on : on);
            }
    }
}

void draw_bitmap(const BitmapCmd& b) {
    if (b.bits == nullptr) return;
    const int stride = (b.src_w + 7) / 8;
    const int ox = b.box.x + (b.box.w - b.src_w) / 2;  // centred in its box
    const int oy = b.box.y + (b.box.h - b.src_h) / 2;
    for (int row = 0; row < b.src_h; ++row)
        for (int col = 0; col < b.src_w; ++col) {
            const uint8_t byte = b.bits[row * stride + (col >> 3)];
            if ((byte >> (7 - (col & 7))) & 1) set_pixel(ox + col, oy + row, true);
        }
}

// ---------------------------------------------------------------- backend

class CrowPanelDisplay : public Display {
public:
    // Exposed so a key on the console can redo it when ghosting builds up.
    void deep_clean(int cycles) { clear_panel(cycles); }

    bool init() override {
        // IO7 first and always: without it the panel never answers and every
        // later symptom points somewhere misleading.
        gpio_config_t out = {};
        out.pin_bit_mask = (1ULL << kPanelPower) | (1ULL << kPanelDc) | (1ULL << kPanelRes);
        out.mode = GPIO_MODE_OUTPUT;
        ESP_ERROR_CHECK(gpio_config(&out));
        gpio_set_level(kPanelPower, 1);
        gpio_set_level(kPanelDc, 1);
        gpio_set_level(kPanelRes, 1);

        gpio_config_t in = {};
        in.pin_bit_mask = 1ULL << kPanelBusy;
        in.mode = GPIO_MODE_INPUT;
        ESP_ERROR_CHECK(gpio_config(&in));
        vTaskDelay(pdMS_TO_TICKS(50));  // let the rail settle

        spi_bus_config_t bus = {};
        bus.mosi_io_num = kPanelMosi;
        bus.miso_io_num = -1;
        bus.sclk_io_num = kPanelClk;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = kHalfScreen + 64;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

        spi_device_interface_config_t dev = {};
        dev.clock_speed_hz = 10 * 1000 * 1000;
        dev.mode = 0;
        dev.spics_io_num = kPanelCs;
        dev.queue_size = 1;
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &g_spi));

        memset(g_fb, 0xFF, sizeof(g_fb));  // all ones is white
        memset(g_fb_prev, 0xFF, sizeof(g_fb_prev));
        panel_init();
        g_ready = true;
        // The board arrives with the vendor's demo on the glass and e-paper keeps
        // it until something drives the pixels the other way. Clear once at boot.
        const int64_t t0 = esp_timer_get_time();
        clear_panel(2);
        ESP_LOGI(kTag, "panel cleared in %lld ms", (esp_timer_get_time() - t0) / 1000);
        ESP_LOGI(kTag, "CrowPanel 5.79 ready: %dx%d visible, %u byte framebuffer",
                 kVisibleW, kHeight, static_cast<unsigned>(sizeof(g_fb)));
        return true;
    }

    Caps caps() const override {
        Caps c;
        c.logical_w = 800;
        c.logical_h = 480;
        c.visible_h = kHeight;
        c.gray_levels = 2;
        c.supports_partial = true;
        return c;
    }

    TextMetrics metrics(TextStyle style) const override {
        if (style == TextStyle::Status)
            return TextMetrics{font::kStatusWidth, font::kStatusHeight};
        return TextMetrics{font::kBodyWidth, font::kBodyHeight};
    }

    void clear() override { memset(g_fb, 0xFF, sizeof(g_fb)); }

    void draw(const DisplayList& list) override {
        clear();
        if (list.dropped())
            ESP_LOGW(kTag, "%u commands dropped: the screen is too complex", list.dropped());
        for (uint8_t i = 0; i < list.size(); ++i) {
            const DrawCmd& c = list[i];
            switch (c.kind) {
                case CmdKind::Text: draw_text(c.text); break;
                case CmdKind::Rect:
                    fill_rect(c.rect.box.x, c.rect.box.y, c.rect.box.w, c.rect.box.h,
                              c.rect.gray < 128);
                    break;
                case CmdKind::Line:
                    fill_rect(c.line.x0, c.line.y0, c.line.x1 - c.line.x0 + 1,
                              c.line.y1 - c.line.y0 + 1 > 0 ? c.line.y1 - c.line.y0 + 1 : 2,
                              true);
                    break;
                case CmdKind::Bitmap: draw_bitmap(c.bitmap); break;
            }
        }
    }

    void present(Refresh intent, PresentedFn on_presented, void* ctx) override {
        if (!g_ready) return;
        const int64_t t0 = esp_timer_get_time();
        // Partial refresh only works once a baseline has been laid down with a
        // full one, so the first present is always full whatever was asked for.
        const bool force_full = !g_baseline_done;
        push_framebuffer();
        uint8_t mode = 0xF7;  // full
        const char* what = "full";
        if (!force_full && intent == Refresh::Partial) {
            mode = 0xDC;
            what = "partial";
        } else if (!force_full && intent == Refresh::Fast) {
            mode = 0xC7;
            what = "fast";
        }
        update(mode);
        g_baseline_done = true;
        const int64_t ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(kTag, "present %s: %lld ms", what, ms);
        if (on_presented)
            on_presented(ctx, static_cast<uint32_t>(esp_timer_get_time() / 1000));
    }

    void fill_pattern(int which) {
        memset(g_fb, 0xFF, sizeof(g_fb));
        switch (which) {
            case 0: memset(g_fb, 0x00, sizeof(g_fb)); break;
            case 1: break;  // left white
            case 2: for (int y = 0; y < kHeight; ++y)
                        for (int x = 0; x < kSeamAt; ++x) set_pixel(x, y, true);
                    break;
            case 3: for (int y = 0; y < kHeight; ++y)
                        for (int x = kSeamAt; x < kVisibleW; ++x) set_pixel(x, y, true);
                    break;
            case 4: for (int y = 0; y < kHeight / 2; ++y)
                        for (int x = 0; x < kVisibleW; ++x) set_pixel(x, y, true);
                    break;
            default: fill_rect(0, 0, 120, 120, true); break;
        }
        const int64_t t0 = esp_timer_get_time();
        push_framebuffer();
        update(0xF7);
        ESP_LOGI(kTag, "pattern %d pushed, full refresh %lld ms", which,
                 (esp_timer_get_time() - t0) / 1000);
    }

    void sleep() override {
        cmd(0x10);
        dat(0x01);
        vTaskDelay(pdMS_TO_TICKS(5));
        // Deep sleep invalidates the panel's RAM, so the next wake needs a
        // hardware reset, a fresh init and a full refresh before partial works.
        g_baseline_done = false;
        ESP_LOGI(kTag, "panel asleep, needs a hardware reset to wake");
    }
};

CrowPanelDisplay g_display;

}  // namespace

Display& display() { return g_display; }
const char* panel_name() { return "CrowPanel 5.79 (development board)"; }
void deep_clean(int cycles) { g_display.deep_clean(cycles); }
void debug_pattern(int which) { g_display.fill_pattern(which); }

}  // namespace halo::board
