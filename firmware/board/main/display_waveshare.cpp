// Waveshare ESP32-S3 e-Paper 3.97 backend: the badge, the actual product.
// 800 x 480, one controller, 1 bit black and white.
//
// The register sequence, the waveform modes and the partial refresh windowing
// are taken from Waveshare's own ESP-IDF examples
// (github.com/waveshareteam/ESP32-S3-ePaper-3.97,
// ESP-IDF/01_E-Paper_Example/components/epaper_port/epaper_port.c and the
// AXP2101 handling in 08_ESP32-S3_e-Paper-3.97), for the same reason the
// development board's driver follows Elecrow's: an e-paper init sequence is not
// something to reconstruct from a family datasheet. What is ours is the
// framebuffer, the rasteriser, the dirty region tracking and the mapping from
// the logical canvas.
//
// How this panel differs from the development board's, beyond the obvious:
//
//   - There is NO GPIO panel power enable. The panel rail is the PMU's ALDO3,
//     reached over I2C. The vendor's integrated demo enables it before every
//     init and disables it in sleep; their standalone e-paper demo never
//     touches it. So either it defaults on at power up or that demo only works
//     on a board some other firmware already configured. We enable it
//     explicitly and log what it was before, which settles the question on the
//     first boot instead of leaving it as folklore.
//
//   - One controller, not two. No slave command set, no 8 dead columns at a
//     seam, no transpose: the framebuffer is row major and goes out as is.
//
//   - 0xF7 is this controller's FULL UPDATE waveform and renders an arbitrary
//     image correctly. On the development board 0xF7 is a global clear that
//     cannot, which is why that driver draws everything with 0xDC. Do not carry
//     that rule across: it is a CrowPanel fact, not an e-paper fact.
//
//   - Partial refresh is region based. The buffer is sized to the region and
//     both x and width must be multiples of 8, because the controller addresses
//     columns a byte at a time.
//
//   - A set bit is WHITE, same as the development board. Drawing black clears
//     bits.
#include <cstring>

#include "board_pins.hpp"
#include "display_backends.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts/font_body.hpp"
#include "fonts/font_status.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace halo::board {
namespace {

constexpr const char* kTag = "panel";

constexpr int kWidth = 800;
constexpr int kHeight = 480;
constexpr int kStride = kWidth / 8;             // 100 bytes per row
constexpr size_t kFbBytes = kStride * kHeight;  // 48000

// 1 = white, 0 = black, so a cleared screen is all ones.
uint8_t* g_fb = nullptr;
// What is on the glass now. Two jobs: working out which region changed, and
// being sent to the controller's previous image RAM (0x26) on every partial.
//
// It was briefly assumed this panel kept 0x26 in sync by itself once a baseline
// had been laid down, so that only 0x24 needed writing. IT DOES NOT, and the
// symptom is unmistakable once seen: sharp leftover glyphs from an earlier
// screen, every time, wherever the old screen had ink that neither the baseline
// nor the new screen has. 0xFF is a differential waveform and drives a pixel
// only where 0x24 and 0x26 disagree, so a stale 0x26 means the controller
// believes those pixels are already correct and never drives them white.
// Caught by eye on the badge 2026-10-27, which no log could have shown.
//
// This is the same trap the development board's driver documents at length. The
// two panels differ in almost everything else; they do not differ in this.
uint8_t* g_fb_prev = nullptr;


spi_device_handle_t g_spi = nullptr;
i2c_master_bus_handle_t g_i2c = nullptr;
i2c_master_dev_handle_t g_pmu = nullptr;
bool g_ready = false;
// Partial refresh is differential against a baseline the controller holds. Until
// one has been laid down with a full or fast base update, a partial has nothing
// to be a difference from, so the first screen after boot or after sleep is
// always a full one.
bool g_baseline_done = false;
// Full screen updates since the last deep clean. Residue builds up over a run of
// them and a full refresh on its own stops being able to cover it, so the clean
// is spent every so often rather than only at boot. Eight is inside the vendor's
// "every 5 to 10" guidance for this panel.
int g_since_clean = 0;
// A safety net, not the schedule. The cadence that matters comes from the
// refresh policy above the display boundary, which asks for a Full on WALLPAPER,
// on BOOT / RECOVERY and every tenth screen, and a Full is a cleaned draw here.
// Counting presents rather than user visible screens is why 8 was far too eager:
// one command is five or six presents (phone check, recording, finalising,
// processing, the result), so it cleaned roughly every other command and turned
// a 2.6 s screen into a 7.7 s one for no reason.
constexpr int kCleanEvery = 40;

// ---------------------------------------------------------------- the PMU
//
// Just enough AXP2101 to own the panel rail. The vendor drives it with
// XPowersLib, about 3000 lines of C++ for a part we need three registers of;
// the register numbers below are from that library's own
// components/axpPower/src/REG/AXP2101Constants.h and XPowersAXP2101.tpp, so
// this is the same arithmetic without the dependency.
//
// F4 (power and battery) needs more of this part than the panel does: the fuel
// gauge percentage is register 0xA4 and is what finally gives CRITICAL BATTERY
// a real source. When that lands this belongs in its own file rather than
// inside the display backend; it lives here for now because the panel cannot
// come up without it.
constexpr uint8_t kRegLdoOnOff0 = 0x90;  // bit 2 is ALDO3
constexpr uint8_t kRegAldo3Vol = 0x94;   // low 5 bits, 100 mV steps from 500 mV
constexpr uint8_t kAldo3Bit = 2;
constexpr uint16_t kPanelMilliVolts = 3300;

bool pmu_read(uint8_t reg, uint8_t* value) {
    return g_pmu != nullptr &&
           i2c_master_transmit_receive(g_pmu, &reg, 1, value, 1, 100) == ESP_OK;
}

bool pmu_write(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return g_pmu != nullptr && i2c_master_transmit(g_pmu, buf, 2, 100) == ESP_OK;
}

// Returns false if the PMU could not be reached at all, which on this board
// means the panel will not light up and nothing further is worth trying.
bool pmu_init() {
    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = kI2cSda;
    bus.scl_io_num = kI2cScl;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus, &g_i2c) != ESP_OK) {
        ESP_LOGE(kTag, "I2C bus init failed on SDA %d / SCL %d", kI2cSda, kI2cScl);
        return false;
    }

    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = kAxp2101Addr;
    dev.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(g_i2c, &dev, &g_pmu) != ESP_OK) {
        ESP_LOGE(kTag, "could not add the PMU at 0x%02x to the I2C bus", kAxp2101Addr);
        return false;
    }

    uint8_t onoff = 0;
    if (!pmu_read(kRegLdoOnOff0, &onoff)) {
        ESP_LOGE(kTag, "no answer from the AXP2101 at 0x%02x: the panel rail "
                       "(ALDO3) cannot be switched on, so the screen will stay blank",
                 kAxp2101Addr);
        return false;
    }

    // The answer to the question the vendor's two demos disagree about, printed
    // once per boot rather than guessed at.
    const bool was_on = (onoff >> kAldo3Bit) & 1;
    ESP_LOGI(kTag, "AXP2101 found, ALDO3 (panel rail) was %s at boot",
             was_on ? "ALREADY ON" : "OFF");

    uint8_t vol = 0;
    pmu_read(kRegAldo3Vol, &vol);
    vol = static_cast<uint8_t>((vol & 0xE0) | ((kPanelMilliVolts - 500) / 100));
    pmu_write(kRegAldo3Vol, vol);
    pmu_write(kRegLdoOnOff0, static_cast<uint8_t>(onoff | (1u << kAldo3Bit)));
    vTaskDelay(pdMS_TO_TICKS(20));  // let the rail come up before the reset
    return true;
}

void pmu_panel_rail(bool on) {
    uint8_t onoff = 0;
    if (!pmu_read(kRegLdoOnOff0, &onoff)) return;
    if (on) onoff = static_cast<uint8_t>(onoff | (1u << kAldo3Bit));
    else onoff = static_cast<uint8_t>(onoff & ~(1u << kAldo3Bit));
    pmu_write(kRegLdoOnOff0, onoff);
}

// ---------------------------------------------------------------- transport

void wait_busy() {
    // BUSY high means busy. The vendor delays 100 ms before even looking, which
    // matters: the pin does not go high the instant the update is asked for, so
    // polling immediately can see the previous idle and return at once.
    vTaskDelay(pdMS_TO_TICKS(100));
    const int64_t deadline = esp_timer_get_time() + 20 * 1000 * 1000;
    while (gpio_get_level(kPanelBusy) != 0) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(kTag, "BUSY stuck high for 20 s: the panel is not answering. "
                           "On this board that is the PMU's ALDO3 rail, not a GPIO.");
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

// 4 KB at a time, which is what the vendor uses. The development board needed
// 64 byte pieces because a single large transaction silently delivered only its
// first bytes; that was a DMA configuration problem on that build, and the
// bounded loop costs nothing here either way.
constexpr size_t kSpiChunk = 4096;

void dat_block(const uint8_t* p, size_t n) {
    while (n > 0) {
        const size_t take = n < kSpiChunk ? n : kSpiChunk;
        spi_send(p, take);
        p += take;
        n -= take;
    }
}

// ---------------------------------------------------------------- init

void hw_reset() {
    gpio_set_level(kPanelRes, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(kPanelRes, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(kPanelRes, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

// Window and address counter for a whole screen update, exactly as the vendor
// sets them: X increments, Y decrements, X 0..799, Y 479..0.
//
// Their Y address counter (0x4F) is set to 0 while the Y window starts at 479,
// which does not look self consistent and looked like the thing to suspect if
// the image came out vertically flipped or shifted by a row.
//
// It is correct. VERIFIED ON THE BADGE 2026-10-27: the full panel probe drew
// all 12 rows with A at the top and L at the bottom, reading left to right,
// with all four corner blocks present. So framebuffer row 0 is the TOP of the
// glass and no flip or mirror is needed, exactly as on the development board.
// Do not "fix" this to match the window: it already matches the hardware.
// `debug_pattern(5)` draws a black square at the origin if it ever needs
// re-checking.
void set_window_full() {
    cmd(0x11); dat(0x01);                       // data entry: X up, Y down
    cmd(0x44); dat(0x00); dat(0x00);            // X start 0
               dat((kWidth - 1) % 256); dat((kWidth - 1) / 256);
    cmd(0x45); dat((kHeight - 1) % 256); dat((kHeight - 1) / 256);
               dat(0x00); dat(0x00);
    cmd(0x4E); dat(0x00); dat(0x00);
    cmd(0x4F); dat(0x00); dat(0x00);
}

void panel_init() {
    vTaskDelay(pdMS_TO_TICKS(10));
    hw_reset();
    wait_busy();
    cmd(0x12);  // SWRESET
    wait_busy();

    cmd(0x18); dat(0x80);              // use the built in temperature sensor
    cmd(0x0C); dat(0xAE); dat(0xC7); dat(0xC3); dat(0xC0); dat(0x80);  // booster
    cmd(0x01); dat((kHeight - 1) % 256); dat((kHeight - 1) / 256); dat(0x02);
    cmd(0x3C); dat(0x01);              // border waveform
    set_window_full();
    wait_busy();
}

// The fast waveform. Same registers in a different order, plus 0x1A = 0x6A,
// which is what selects it.
void panel_init_fast() {
    vTaskDelay(pdMS_TO_TICKS(10));
    hw_reset();
    wait_busy();
    cmd(0x12);  // SWRESET
    wait_busy();

    cmd(0x0C); dat(0xAE); dat(0xC7); dat(0xC3); dat(0xC0); dat(0x80);
    cmd(0x01); dat((kHeight - 1) % 256); dat((kHeight - 1) / 256); dat(0x02);
    set_window_full();
    wait_busy();
    cmd(0x3C); dat(0x01);
    cmd(0x18); dat(0x80);
    cmd(0x1A); dat(0x6A);              // fast mode
}

void update(uint8_t mode) {
    cmd(0x22);
    dat(mode);
    cmd(0x20);
    wait_busy();
}

// Region partial refresh works on this panel, at about 620 ms against 2573 for
// a full one, so there is no size limit worth imposing: the whole panel can be
// a partial.
//
// It took most of an evening to establish, because it was broken by our own
// 0x26 write (see push_region) and every wrong result still reported a healthy
// 620 ms refresh. What the panel actually settled, by drawing labelled bands
// and looking at them:
//
//   0x45 must be sent HIGH value first, the Y-decrement convention, which is
//   what the full init uses and what Waveshare's ESP-IDF port uses. Their
//   Arduino port sends it low first and does not draw at all.
//
//   0x44 and 0x4E are in PIXELS, not byte addresses. That much is settled, but
//   a window narrower than the panel still comes out shifted, so we do not use
//   one: present() widens every region to the full row and clips only in Y.
//   The X window remains the one part of this sequence that is not understood,
//   and it is avoided rather than guessed at.
//
//   0x11 is set explicitly to Y-decrement after the reset. The panel also works
//   without it, on the power on default, but the vendor's own partial programs
//   a decrement style window into a chip it leaves incrementing, and relying on
//   that is not worth the saving of one command.
//
// Ghosting still accumulates over a run of partials. The refresh policy above
// the display boundary spends a Full every tenth screen, and a Full here is a
// cleaned draw, which is what keeps the glass honest.
constexpr uint8_t kUpdateFull = 0xF7;
constexpr uint8_t kUpdateFast = 0xD7;
constexpr uint8_t kUpdatePartial = 0xFF;

// Lay down a baseline: the same image into the working RAM (0x24) and the
// previous image RAM (0x26), so the controller knows what is on the glass and
// every later partial can be a difference from it.
void push_whole_screen(bool as_baseline, uint8_t waveform) {
    cmd(0x24);
    dat_block(g_fb, kFbBytes);
    if (as_baseline) {
        cmd(0x26);
        dat_block(g_fb, kFbBytes);
    }
    update(waveform);
    memcpy(g_fb_prev, g_fb, kFbBytes);
    if (as_baseline) g_baseline_done = true;
}

// A region of the current framebuffer, sent on its own. x and width are in
// whole bytes because the controller addresses columns a byte at a time; the
// caller has already aligned them.
//
// Sent row by row straight out of the framebuffer rather than gathered into a
// scratch buffer first: the address counter advances across the window by
// itself, so consecutive writes concatenate, and a third 48 KB buffer would
// cost more than the handful of extra SPI transactions.
void push_region(int byte_x0, int byte_x1, int y0, int y1) {
    const int x_start = byte_x0 * 8;
    const int x_end = (byte_x1 - 1) * 8;

    hw_reset();
    cmd(0x18); dat(0x80);
    cmd(0x3C); dat(0x80);              // border waveform for a partial update

    // THE DATA ENTRY MODE MUST BE SET AGAIN HERE. The hardware reset above puts
    // 0x11 back to its power on default, which counts Y UPWARDS, while the full
    // refresh path sets it to 0x01, which counts Y DOWNWARDS. The vendor's
    // partial never sets it, so a region handed over as rows 4 to 24 was drawn
    // 4 to 24 rows from the BOTTOM of the glass instead of the top: vertically
    // mirrored, and silently, because the driver's own log reported the region
    // it intended rather than the region the panel used.
    //
    // Caught by eye on the badge 2026-10-27: a status bar update that should
    // have replaced "Ready to confirm" at the top appeared along the bottom edge
    // with the old status bar untouched. A mirrored region also looks exactly
    // like leftover ink from an older screen, so this was part of what made the
    // panel appear to stack screens on top of each other.
    cmd(0x11); dat(0x01);

    cmd(0x44); dat(x_start & 0xFF); dat((x_start >> 8) & 0xFF);
               dat(x_end & 0xFF); dat((x_end >> 8) & 0xFF);
    cmd(0x45); dat((y1 - 1) & 0xFF); dat(((y1 - 1) >> 8) & 0xFF);
               dat(y0 & 0xFF); dat((y0 >> 8) & 0xFF);
    cmd(0x4E); dat(x_start & 0xFF); dat((x_start >> 8) & 0xFF);
    cmd(0x4F); dat(y0 & 0xFF); dat((y0 >> 8) & 0xFF);

    const size_t run = static_cast<size_t>(byte_x1 - byte_x0);

    // ONLY 0x24. Do not touch 0x26 here.
    //
    // This cost an entire evening. The development board's controller needs the
    // previous image pushed into its old-image RAM on every present, and that
    // rule was carried across to this panel on the assumption that an e-paper
    // controller is an e-paper controller. It is not. Writing 0x26 during a
    // partial on this panel makes the refresh render the OLD content, so the
    // screen being replaced stays readable underneath the new one, and a small
    // region appears to do nothing at all.
    //
    // Proved on the glass 2026-10-27 with two identical partial bands over a
    // panel full of 'o': the band that wrote 0x26 kept the 'o' showing through,
    // the band that did not was clean. The vendor never writes it. Neither do
    // we. The controller maintains its own previous image from the baseline
    // that EPD_Display_Base laid down, which is what push_whole_screen() does.
    cmd(0x24);
    for (int y = y0; y < y1; ++y) dat_block(g_fb + y * kStride + byte_x0, run);

    update(kUpdatePartial);
    memcpy(g_fb_prev, g_fb, kFbBytes);
}

// Has anything changed since what is on the glass? That is the only question
// asked of the framebuffer now.
//
// This used to return a bounding box of the changed bytes, so a partial could be
// clipped to it. It cannot: this panel places a sub-region wrong in both axes
// (see push_region), and clipping would save about 20 ms out of 655 because the
// refresh time is in the waveform rather than the bytes. So the box went and the
// boolean stayed. Worth skipping, though: a refresh that moves no pixels still
// takes two thirds of a second and still ages the panel.
bool anything_changed() {
    return memcmp(g_fb, g_fb_prev, kFbBytes) != 0;
}

void deep_sleep() {
    cmd(0x10);
    dat(0x01);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// Stream a constant into one of the RAMs without touching the framebuffer. The
// clear used to memset g_fb, which was fine when it only ran at boot and is not
// fine now that it runs between a draw and its present: it would wipe the screen
// that had just been rasterised and push a blank panel instead.
void fill_ram(uint8_t reg, uint8_t value) {
    static uint8_t block[256];
    memset(block, value, sizeof(block));
    cmd(reg);
    size_t left = kFbBytes;
    while (left > 0) {
        const size_t take = left < sizeof(block) ? left : sizeof(block);
        dat_block(block, take);
        left -= take;
    }
}

// Drive every pixel hard between the two extremes a few times, to flush the
// residue a run of refreshes leaves behind. Always finishes white.
//
// It is needed. Confirmed on the badge 2026-10-27: after a long session of
// screens, a full refresh alone could no longer render cleanly over what was
// already on the glass, and older screens stayed readable underneath. One deep
// clean fixed it and the next screen was perfect. So this runs periodically,
// not only at boot.
void clear_panel(int cycles) {
    if (cycles < 1) cycles = 1;
    if (cycles % 2) ++cycles;
    for (int i = 0; i < cycles; ++i) {
        panel_init();
        const uint8_t now = (i % 2) ? 0xFF : 0x00;  // black first, ending white
        fill_ram(0x24, now);
        fill_ram(0x26, now);
        update(kUpdateFull);
    }
    // The glass is white now; the framebuffer is left exactly as it was.
    memset(g_fb_prev, 0xFF, kFbBytes);
    g_baseline_done = true;
}

// ---------------------------------------------------------------- raster

inline void set_pixel(int x, int y, bool ink) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    uint8_t& byte = g_fb[y * kStride + (x >> 3)];
    const uint8_t bit = static_cast<uint8_t>(0x80 >> (x & 7));
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
                set_pixel(x + bit, c.box.y + row, c.invert ? !on : on);
            }
    }
}

inline bool bitmap_ink(const BitmapCmd& b, int stride, int x, int y) {
    const uint8_t byte = b.bits[y * stride + (x >> 3)];
    const bool on = (byte >> (7 - (x & 7))) & 1;
    return b.invert ? !on : on;
}

void draw_bitmap(const BitmapCmd& b) {
    if (b.bits == nullptr || b.src_w == 0 || b.src_h == 0) return;
    const int stride = (b.src_w + 7) / 8;

    if (b.scale_to_box && b.box.w > 0 && b.box.h > 0) {
        // The wallpaper path. Layout hands over the badge's 800 x 480 canvas and
        // this panel is exactly that, so unlike the development board nothing is
        // squeezed: the scaling below is an identity in the normal case and is
        // kept only so an odd sized image still lands.
        const int dst_w = b.box.x + b.box.w > kWidth ? kWidth - b.box.x : b.box.w;
        const int dst_h = b.box.y + b.box.h > kHeight ? kHeight - b.box.y : b.box.h;
        for (int y = 0; y < dst_h; ++y) {
            const int sy = static_cast<int>(static_cast<uint32_t>(y) * b.src_h / dst_h);
            for (int x = 0; x < dst_w; ++x) {
                const int sx = static_cast<int>(static_cast<uint32_t>(x) * b.src_w / dst_w);
                set_pixel(b.box.x + x, b.box.y + y, bitmap_ink(b, stride, sx, sy));
            }
        }
        return;
    }

    const int ox = b.box.x + (b.box.w - b.src_w) / 2;
    const int oy = b.box.y + (b.box.h - b.src_h) / 2;
    for (int row = 0; row < b.src_h; ++row)
        for (int col = 0; col < b.src_w; ++col)
            if (bitmap_ink(b, stride, col, row)) set_pixel(ox + col, oy + row, true);
}


// ---------------------------------------------------------------- partial probe
//
// Which partial refresh convention does this panel actually obey?
//
// Waveshare's two ports of this same panel disagree with each other, and the
// disagreement is exactly the thing that was getting regions misplaced:
//
//   ESP-IDF  0x45 <- (Yend, Ystart)   high first, the Y-DECREMENT convention
//   Arduino  0x45 <- (Ystart, Yend)   low first,  the Y-INCREMENT convention
//
// Neither sets 0x11 after the reset inside the partial, so the chip is left at
// its power on default, which is Y-INCREMENT. That makes the Arduino port
// self consistent and the ESP-IDF port internally contradictory: it programs a
// decrement style window into a chip that is incrementing. We copied the
// ESP-IDF one.
//
// Rather than argue from datasheets about which is right, draw all of them and
// look. Each variant gets its own band of the panel, each band is filled with
// its own letter and has a solid black bar across the TOP eight rows, so one
// photograph answers three questions at once: did it draw at all, did it land
// at the right height, and is it upside down inside its own band.
struct PartialVariant {
    char tag;
    int entry_mode;   // -1 leave at the power on default, else the value for 0x11
    bool high_first;  // 0x45 order
    bool counter_high;
    int write_old;    // 0 none, 1 known white, 2 from g_fb_prev
    bool x_in_bytes;  // 0x44 and 0x4E in byte addresses rather than pixels
    const char* what;
};

void push_region_as(const PartialVariant& v, int byte_x0, int byte_x1, int y0, int y1) {
    // The vendor multiplies its byte column by 8 and calls that the X address.
    // The development board's controller takes 0x44 in BYTES (its driver sends
    // 0x00 to 0x31 for 400 pixels), and a full width region cannot tell the two
    // apart, because a pixel valued end address is far out of range and simply
    // clamps to the whole row. A narrow region can.
    const int x_start = v.x_in_bytes ? byte_x0 : byte_x0 * 8;
    const int x_end = v.x_in_bytes ? (byte_x1 - 1) : (byte_x1 - 1) * 8;
    const int hi = y1 - 1, lo = y0;

    hw_reset();
    cmd(0x18); dat(0x80);
    cmd(0x3C); dat(0x80);
    if (v.entry_mode >= 0) { cmd(0x11); dat(static_cast<uint8_t>(v.entry_mode)); }

    cmd(0x44); dat(x_start & 0xFF); dat((x_start >> 8) & 0xFF);
               dat(x_end & 0xFF); dat((x_end >> 8) & 0xFF);
    cmd(0x45);
    if (v.high_first) { dat(hi & 0xFF); dat((hi >> 8) & 0xFF); dat(lo & 0xFF); dat((lo >> 8) & 0xFF); }
    else              { dat(lo & 0xFF); dat((lo >> 8) & 0xFF); dat(hi & 0xFF); dat((hi >> 8) & 0xFF); }

    const int cy = v.counter_high ? hi : lo;
    cmd(0x4E); dat(x_start & 0xFF); dat((x_start >> 8) & 0xFF);
    cmd(0x4F); dat(cy & 0xFF); dat((cy >> 8) & 0xFF);

    const size_t run = static_cast<size_t>(byte_x1 - byte_x0);
    cmd(0x24);
    for (int y = y0; y < y1; ++y) dat_block(g_fb + y * kStride + byte_x0, run);

    if (v.write_old != 0) {
        cmd(0x4E); dat(x_start & 0xFF); dat((x_start >> 8) & 0xFF);
        cmd(0x4F); dat(cy & 0xFF); dat((cy >> 8) & 0xFF);
        cmd(0x26);
        static uint8_t row[kStride];
        for (int y = y0; y < y1; ++y) {
            if (v.write_old == 1) memset(row, 0xFF, sizeof(row));
            else memcpy(row, g_fb_prev + y * kStride + byte_x0, run);
            dat_block(row, run);
        }
    }

    update(kUpdatePartial);
}

void partial_probe() {
    if (!g_ready) return;

    // A known, clean, entirely white baseline. Anything black afterwards was put
    // there by a partial refresh and nothing else.
    clear_panel(2);
    memset(g_fb, 0xFF, kFbBytes);
    panel_init();
    push_whole_screen(true, kUpdateFull);
    ESP_LOGI(kTag, "partial probe: white baseline laid down, now five variants");

    static const PartialVariant kVariants[] = {
        {'A', -1,   true,  false, 0, false, "vendor ESP-IDF exactly as shipped"},
        {'B', -1,   false, false, 0, false, "vendor Arduino exactly as shipped"},
        {'C', 0x01, true,  false, 0, false, "forced Y-decrement, high-first window"},
        {'D', 0x03, false, false, 0, false, "forced Y-increment, low-first window"},
        {'E', 0x01, true,  false, 1, false, "as C, plus the old image pushed to 0x26"},
    };

    int y = 20;
    for (const PartialVariant& v : kVariants) {
        const int y0 = y, y1 = y + 70;
        memset(g_fb, 0xFF, kFbBytes);
        fill_rect(0, y0, kWidth, 8, true);          // bar across the TOP of the band
        char line[49];
        for (int i = 0; i < 48; ++i) line[i] = v.tag;
        line[48] = '\0';
        TextCmd t;
        t.box = Box{16, static_cast<int16_t>(y0 + 20), static_cast<int16_t>(kWidth - 32), 32};
        t.text = line;
        draw_text(t);

        const int64_t t0 = esp_timer_get_time();
        push_region_as(v, 0, kStride, y0, y1);
        ESP_LOGI(kTag, "  %c: rows %d..%d, %lld ms, %s", v.tag, y0, y1,
                 (esp_timer_get_time() - t0) / 1000, v.what);
        y += 90;
    }

    memset(g_fb, 0xFF, kFbBytes);
    memset(g_fb_prev, 0xFF, kFbBytes);
    ESP_LOGI(kTag, "partial probe done. Each band should show its own letter across "
                   "the full width with a black bar along the TOP of the band. "
                   "A missing band did not draw; a band whose bar is at the BOTTOM "
                   "is upside down; a band at the wrong height is misplaced.");
}


// The X half of the same question, which the first probe could not ask because
// every one of its regions was the full width of the panel.
//
// Each band here is narrow: byte columns 25 to 75, which is pixels 200 to 600,
// the middle half of the screen. A band whose letters sit in the middle is
// correctly addressed. A band that is shifted, clipped to one side, or missing
// is not.
void partial_probe_x() {
    if (!g_ready) return;

    clear_panel(2);
    memset(g_fb, 0xFF, kFbBytes);
    panel_init();
    push_whole_screen(true, kUpdateFull);
    ESP_LOGI(kTag, "partial X probe: white baseline, now three narrow regions");

    static const PartialVariant kVariants[] = {
        {'F', 0x01, true, false, 0, false, "X as PIXELS, what the driver does now"},
        {'G', 0x01, true, false, 0, true,  "X as BYTE addresses"},
        {'H', -1,   true, false, 0, true,  "X as bytes, 0x11 left at the default"},
    };

    constexpr int kX0 = 25, kX1 = 75;   // pixels 200 to 600
    int y = 60;
    for (const PartialVariant& v : kVariants) {
        const int y0 = y, y1 = y + 70;
        memset(g_fb, 0xFF, kFbBytes);
        fill_rect(kX0 * 8, y0, (kX1 - kX0) * 8, 8, true);
        char line[26];
        for (int i = 0; i < 25; ++i) line[i] = v.tag;
        line[25] = '\0';
        TextCmd t;
        t.box = Box{static_cast<int16_t>(kX0 * 8), static_cast<int16_t>(y0 + 20),
                    static_cast<int16_t>((kX1 - kX0) * 8), 32};
        t.text = line;
        draw_text(t);

        push_region_as(v, kX0, kX1, y0, y1);
        ESP_LOGI(kTag, "  %c: rows %d..%d, x pixels %d..%d, %s", v.tag, y0, y1,
                 kX0 * 8, kX1 * 8, v.what);
        y += 130;
    }

    memset(g_fb, 0xFF, kFbBytes);
    memset(g_fb_prev, 0xFF, kFbBytes);
    ESP_LOGI(kTag, "X probe done. A correct band is a block of its own letter in "
                   "the MIDDLE HALF of the panel, bar along its top, nothing in "
                   "the left or right quarter.");
}


// The last difference between the probe that works and the driver that does
// not: the driver pushes the previous image into 0x26 on every partial, and the
// probe never did.
//
// This asks the question against a realistic starting point rather than a blank
// panel. The whole screen is first filled with OLD, by a full refresh, so the
// glass genuinely holds a previous screen. Then two bands are replaced by a
// partial: one the way the driver does it, one the way the working probe does.
void partial_probe_old() {
    if (!g_ready) return;

    clear_panel(2);
    memset(g_fb, 0xFF, kFbBytes);
    char oldline[49];
    for (int i = 0; i < 48; ++i) oldline[i] = 'o';
    oldline[48] = '\0';
    for (int row = 0; row < 12; ++row) {
        TextCmd t;
        t.box = Box{16, static_cast<int16_t>(row * 40 + 4), kWidth - 32, 32};
        t.text = oldline;
        draw_text(t);
    }
    panel_init();
    push_whole_screen(true, kUpdateFull);   // glass and g_fb_prev both hold OLD
    ESP_LOGI(kTag, "0x26 probe: panel filled with 'o', now two partial bands");

    static const PartialVariant kVariants[] = {
        {'K', 0x01, true, false, 2, false, "exactly what the driver does: 0x26 <- previous image"},
        {'L', 0x01, true, false, 0, false, "what the working probe does: 0x24 only, no 0x26"},
    };

    int y = 120;
    for (const PartialVariant& v : kVariants) {
        const int y0 = y, y1 = y + 80;
        // Only the band changes; everything else stays as OLD, which is what a
        // real screen change looks like.
        for (int yy = y0; yy < y1; ++yy)
            memset(g_fb + yy * kStride, 0xFF, kStride);
        char line[49];
        for (int i = 0; i < 48; ++i) line[i] = v.tag;
        line[48] = '\0';
        TextCmd t;
        t.box = Box{16, static_cast<int16_t>(y0 + 24), kWidth - 32, 32};
        t.text = line;
        draw_text(t);

        push_region_as(v, 0, kStride, y0, y1);
        ESP_LOGI(kTag, "  %c: rows %d..%d, %s", v.tag, y0, y1, v.what);
        memcpy(g_fb_prev + y0 * kStride, g_fb + y0 * kStride,
               static_cast<size_t>(y1 - y0) * kStride);
        y += 180;
    }

    ESP_LOGI(kTag, "0x26 probe done. A correct band shows ONLY its own letter, with "
                   "no 'o' left underneath. The rest of the panel stays 'o'.");
}

// ---------------------------------------------------------------- backend

class WaveshareDisplay : public Display {
public:
    void deep_clean(int cycles) {
        if (!g_ready) return;
        clear_panel(cycles);
    }

    bool init() override {
        g_fb = static_cast<uint8_t*>(heap_caps_malloc(kFbBytes, MALLOC_CAP_DMA));
        // Only ever compared against, never sent, so it can live in PSRAM and
        // leave the internal RAM to the radio and the audio buffers.
        g_fb_prev = static_cast<uint8_t*>(heap_caps_malloc(kFbBytes, MALLOC_CAP_SPIRAM));
        if (g_fb_prev == nullptr) g_fb_prev = static_cast<uint8_t*>(malloc(kFbBytes));
        if (g_fb == nullptr || g_fb_prev == nullptr) {
            ESP_LOGE(kTag, "no room for two %u byte framebuffers", static_cast<unsigned>(kFbBytes));
            return false;
        }
        memset(g_fb, 0xFF, kFbBytes);
        memset(g_fb_prev, 0xFF, kFbBytes);

        // The panel rail first and always. On the development board the
        // equivalent mistake is leaving IO7 low; here it is leaving ALDO3 off,
        // and the symptom is identical: every byte is accepted and nothing
        // happens.
        if (!pmu_init()) return false;

        gpio_config_t out = {};
        out.pin_bit_mask = (1ULL << kPanelDc) | (1ULL << kPanelRes);
        out.mode = GPIO_MODE_OUTPUT;
        ESP_ERROR_CHECK(gpio_config(&out));
        gpio_set_level(kPanelDc, 1);
        gpio_set_level(kPanelRes, 1);

        gpio_config_t in = {};
        in.pin_bit_mask = 1ULL << kPanelBusy;
        in.mode = GPIO_MODE_INPUT;
        ESP_ERROR_CHECK(gpio_config(&in));

        spi_bus_config_t bus = {};
        bus.mosi_io_num = kPanelMosi;
        bus.miso_io_num = -1;
        bus.sclk_io_num = kPanelClk;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = kSpiChunk + 64;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

        spi_device_interface_config_t dev = {};
        dev.clock_speed_hz = 20 * 1000 * 1000;
        dev.mode = 0;
        // The vendor leaves CS to a GPIO that is configured as an output and
        // never driven, so it sits low for the life of the board. We let the SPI
        // peripheral own it, which is what the development board's driver does
        // and what the part expects. If the panel never answers, this is the
        // second thing to try after the ALDO3 rail: set this to -1 and drive
        // kPanelCs low by hand.
        dev.spics_io_num = kPanelCs;
        dev.queue_size = 1;
        ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &g_spi));

        g_ready = true;
        // E-paper keeps whatever was on it, and a new board arrives with the
        // vendor's demo on the glass, so clear once at boot.
        const int64_t t0 = esp_timer_get_time();
        clear_panel(2);
        ESP_LOGI(kTag, "panel cleared in %lld ms", (esp_timer_get_time() - t0) / 1000);
        ESP_LOGI(kTag, "Waveshare 3.97 ready: %dx%d, %u byte framebuffer",
                 kWidth, kHeight, static_cast<unsigned>(kFbBytes));
        return true;
    }

    Caps caps() const override {
        Caps c;
        c.logical_w = 800;
        c.logical_h = 480;
        c.visible_h = 480;   // the badge shows the whole canvas
        // 1 bit black and white only (decided 2026-10-13, Victor). The panel can
        // do 4 gray levels, but 4 gray mode has no partial refresh at all on this
        // panel, so every grayscale screen would cost a full refresh.
        c.gray_levels = 2;
        c.supports_partial = true;
        return c;
    }

    TextMetrics metrics(TextStyle style) const override {
        if (style == TextStyle::Status)
            return TextMetrics{font::kStatusWidth, font::kStatusHeight};
        return TextMetrics{font::kBodyWidth, font::kBodyHeight};
    }

    void clear() override { if (g_fb) memset(g_fb, 0xFF, kFbBytes); }

    void draw(const DisplayList& list) override {
        if (!g_ready) return;
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

        // A partial is a difference from a baseline the controller holds. After
        // boot, after a panel sleep, or after a full clear there is no baseline,
        // so the intent is quietly promoted to a full refresh rather than
        // drawing nothing.
        const bool can_partial = g_baseline_done && intent == Refresh::Partial;

        if (can_partial && !anything_changed()) {
            ESP_LOGI(kTag, "present skipped: nothing changed");
            if (on_presented)
                on_presented(ctx, static_cast<uint32_t>(esp_timer_get_time() / 1000));
            return;
        }

        if (can_partial) {
            // The whole panel, every time. The dirty region decided one thing
            // and that decision is already made above.
            push_region(0, kStride, 0, kHeight);
            ESP_LOGI(kTag, "present partial: %d x %d, %lld ms", kWidth, kHeight,
                     (esp_timer_get_time() - t0) / 1000);
            if (on_presented)
                on_presented(ctx, static_cast<uint32_t>(esp_timer_get_time() / 1000));
            return;
        }

        // A deep clean first when one is due, or when the caller asked for a
        // Full on purpose (WALLPAPER and BOOT / RECOVERY, where nobody is
        // waiting on a confirm and the time is free).
        const bool want_clean = intent == Refresh::Full || g_since_clean >= kCleanEvery;
        if (want_clean) {
            clear_panel(2);
            g_since_clean = 0;
        }

        const char* what;
        if (intent == Refresh::Fast && g_baseline_done && !want_clean) {
            panel_init_fast();
            push_whole_screen(true, kUpdateFast);
            what = "fast";
        } else {
            // A standard full refresh, which is the only one that clears
            // ghosting: a fast refresh does not, whatever its name suggests.
            panel_init();
            push_whole_screen(true, kUpdateFull);
            what = want_clean ? "full (cleaned first)" : "full";
        }
        ++g_since_clean;

        ESP_LOGI(kTag, "present %s: %lld ms", what, (esp_timer_get_time() - t0) / 1000);
        if (on_presented)
            on_presented(ctx, static_cast<uint32_t>(esp_timer_get_time() / 1000));
    }

    void fill_pattern(int which) {
        if (!g_ready) return;
        memset(g_fb, 0xFF, kFbBytes);
        switch (which) {
            case 0: memset(g_fb, 0x00, kFbBytes); break;
            case 1: break;  // all white
            case 2: fill_rect(0, 0, kWidth / 2, kHeight, true); break;      // left half
            case 3: fill_rect(kWidth / 2, 0, kWidth / 2, kHeight, true); break;  // right half
            case 4: fill_rect(0, 0, kWidth, kHeight / 2, true); break;      // top half
            // The orientation probe. A square at the origin plus a thin bar down
            // the left edge: where it lands says which corner row 0 column 0 is,
            // which is the one thing about this panel that cannot be settled by
            // reading the vendor's code.
            default:
                fill_rect(0, 0, 120, 120, true);
                fill_rect(0, 0, 8, kHeight, true);
                break;
        }
        const int64_t t0 = esp_timer_get_time();
        panel_init();
        push_whole_screen(true, kUpdateFull);
        ESP_LOGI(kTag, "pattern %d pushed, %lld ms", which,
                 (esp_timer_get_time() - t0) / 1000);
    }

    void sleep() override {
        if (!g_ready) return;
        deep_sleep();
        // On this board sleeping the panel also means dropping its rail, and
        // ALDO3 going away destroys the partial refresh baseline along with the
        // controller's RAM. The next wake needs a fresh init and a full refresh
        // before any partial will land.
        pmu_panel_rail(false);
        g_baseline_done = false;
        ESP_LOGI(kTag, "panel asleep and its rail is off, needs a full refresh to wake");
    }
};

WaveshareDisplay g_display;

}  // namespace

Display& display() { return g_display; }
const char* panel_name() { return "Waveshare 3.97 (the badge)"; }
void deep_clean(int cycles) { g_display.deep_clean(cycles); }
void probe_partial() { partial_probe(); }
void probe_partial_x() { partial_probe_x(); }
void probe_partial_old() { partial_probe_old(); }
void debug_pattern(int which) { g_display.fill_pattern(which); }

}  // namespace halo::board
