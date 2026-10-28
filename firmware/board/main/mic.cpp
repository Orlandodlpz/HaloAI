#include "mic.hpp"

#include <cmath>
#include <initializer_list>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

#include "board_pins.hpp"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

namespace halo::board::mic {
namespace {

constexpr const char* kTag = "halo.mic";
constexpr uint32_t kSampleRate = 16000;   // what everything downstream expects
uint32_t g_rate = kSampleRate;

i2s_chan_handle_t g_rx = nullptr;
i2s_std_slot_mask_t g_slot = I2S_STD_SLOT_LEFT;

// Read in blocks rather than one sample at a time: the DMA hands over buffers,
// and a per sample read would spend its life in the driver.
constexpr size_t kBlockFrames = 256;
int32_t g_block[kBlockFrames];

}  // namespace

bool start_on(i2s_std_slot_mask_t slot, uint32_t rate = kSampleRate) {
#if defined(HALO_PANEL_WAVESHARE)
    // The badge's microphone is behind an ES8311 codec on the shared I2C bus,
    // not a bare I2S microphone like the development board's INMP441. The codec
    // has to be configured over I2C before it clocks out a single sample, and
    // none of that is written yet. Refusing loudly beats bringing I2S up on the
    // codec's pins and recording silence that looks like a dead microphone.
    //
    // Porting this is the badge half of F2: the pin map is already in
    // board_pins.hpp (MCLK 13, BCLK 14, WS 47, DIN 21, codec at I2C 0x18) and
    // everything above this file, the capture task, the ring buffer and the
    // conditioning, carries over unchanged.
    (void)slot; (void)rate;
    ESP_LOGE(kTag, "no microphone on this board yet: the badge needs the ES8311 "
                   "codec brought up over I2C first (F2, badge half)");
    return false;
#else
    if (g_rx != nullptr && slot == g_slot && rate == g_rate) return true;
    stop();
    g_slot = slot;
    g_rate = rate;

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    if (i2s_new_channel(&chan, nullptr, &g_rx) != ESP_OK) {
        ESP_LOGE(kTag, "no free I2S channel");
        return false;
    }

    // 32 bit slots, left only. The INMP441 sends 24 bits MSB aligned inside a
    // 32 bit slot, so a 16 bit slot configuration reads the top of the word and
    // throws away the part that carries quiet speech. With L/R tied to ground it
    // speaks in the left slot and the right slot is zeros, which is why a
    // capture that is half silence is usually the wrong slot, not a dead part.
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(g_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,     // the microphone makes its own from BCLK
            .bclk = kMicBclk,
            .ws = kMicWs,
            .dout = I2S_GPIO_UNUSED,     // receive only
            .din = kMicData,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    cfg.slot_cfg.slot_mask = g_slot;
    if (g_slot == I2S_STD_SLOT_BOTH) cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;

    esp_err_t err = i2s_channel_init_std_mode(g_rx, &cfg);
    if (err == ESP_OK) err = i2s_channel_enable(g_rx);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "I2S would not start: %s", esp_err_to_name(err));
        i2s_del_channel(g_rx);
        g_rx = nullptr;
        return false;
    }
    ESP_LOGI(kTag, "I2S up: %lu Hz (BCLK %lu kHz), BCLK IO%d, WS IO%d, DATA IO%d, %s slot",
             static_cast<unsigned long>(g_rate), static_cast<unsigned long>(g_rate * 64 / 1000),
             kMicBclk, kMicWs, kMicData,
             g_slot == I2S_STD_SLOT_LEFT ? "left"
                                         : (g_slot == I2S_STD_SLOT_RIGHT ? "right" : "both"));
    return true;
#endif
}

bool start() { return start_on(I2S_STD_SLOT_LEFT); }

void stop() {
    if (g_rx == nullptr) return;
    i2s_channel_disable(g_rx);
    i2s_del_channel(g_rx);
    g_rx = nullptr;
}

Reading read(size_t frames) {
    Reading r;
    if (g_rx == nullptr && !start()) return r;
    r.slot = g_slot;

    double sum_squares = 0;
    bool first = true;
    int32_t previous = 0;

    while (r.frames < frames) {
        const size_t want = frames - r.frames < kBlockFrames ? frames - r.frames : kBlockFrames;
        size_t got = 0;
        if (i2s_channel_read(g_rx, g_block, want * sizeof(int32_t), &got, 200) != ESP_OK)
            return r;
        const size_t n = got / sizeof(int32_t);
        if (n == 0) break;

        for (size_t i = 0; i < n; ++i) {
            const int32_t raw = g_block[i];
            if (first || raw < r.raw_min) r.raw_min = raw;
            if (first || raw > r.raw_max) r.raw_max = raw;
            // A line that is not clocking, or not connected, repeats one value
            // forever. Counting the changes separates that from genuine silence,
            // which on a MEMS microphone is noisy at the bottom bits and never
            // bit identical twice running.
            if (!first && raw != previous) ++r.distinct;
            previous = raw;
            first = false;

            // 24 bits MSB aligned in 32: drop the 8 empty low bits, then take the
            // top 16 of what is left.
            const int16_t sample = static_cast<int16_t>(raw >> 16);
            const int16_t magnitude = static_cast<int16_t>(sample < 0 ? -(sample + 1) : sample);
            if (magnitude > r.peak) r.peak = magnitude;
            sum_squares += static_cast<double>(sample) * static_cast<double>(sample);
        }
        r.frames += n;
    }

    if (r.frames > 0) {
        const double rms = std::sqrt(sum_squares / static_cast<double>(r.frames));
        r.rms_dbfs = rms > 0 ? static_cast<float>(20.0 * std::log10(rms / 32768.0)) : -120.f;
        r.ok = true;
    }
    return r;
}

void probe(uint32_t seconds) {
    if (!start()) return;
    ESP_LOGI(kTag, "listening for %lu s. Tap the microphone, or talk at it.",
             static_cast<unsigned long>(seconds));
    ESP_LOGI(kTag, "  level is the loudness. distinct is how many samples differed from "
                   "the one before: a line that is not clocking repeats one value.");

    size_t dead_windows = 0, windows = 0;
    int16_t loudest = 0;
    for (uint32_t i = 0; i < seconds * 5; ++i) {   // 200 ms a window
        const Reading r = read(kSampleRate / 5);
        ++windows;
        if (!r.ok) {
            ESP_LOGE(kTag, "read failed: no data from the I2S driver at all");
            continue;
        }
        if (r.distinct < r.frames / 100) ++dead_windows;
        if (r.peak > loudest) loudest = r.peak;
        ESP_LOGI(kTag, "  level %6.1f dBFS   peak %6d   distinct %4u/%u   raw %ld..%ld",
                 static_cast<double>(r.rms_dbfs), r.peak, static_cast<unsigned>(r.distinct),
                 static_cast<unsigned>(r.frames), static_cast<long>(r.raw_min),
                 static_cast<long>(r.raw_max));
    }

    // Say what it means. A wall of numbers is not a diagnosis.
    if (dead_windows == windows) {
        // An INMP441 with L/R tied to ground speaks in the left slot and the
        // right one is zeros, so a silent left slot could be our configuration
        // rather than the part. Say so only after trying the alternatives: a
        // verdict that blames someone's soldering had better have ruled out the
        // firmware first.
        ESP_LOGI(kTag, "nothing in the left slot. Trying the other slot configurations before "
                       "calling it a wiring fault.");
        const struct { i2s_std_slot_mask_t slot; const char* what; const char* means; } others[] = {
            {I2S_STD_SLOT_RIGHT, "right",
             "L/R is tied to 3.3 V, not ground. Either move it to ground or read the right slot."},
            {I2S_STD_SLOT_BOTH, "both",
             "the microphone is alive but the mono slot configuration is not reading it."},
        };
        for (const auto& o : others) {
            if (!start_on(o.slot)) continue;
            const Reading r = read(kSampleRate / 2);
            ESP_LOGI(kTag, "  %s slot: distinct %u/%u, peak %d", o.what,
                     static_cast<unsigned>(r.distinct), static_cast<unsigned>(r.frames), r.peak);
            if (r.ok && r.distinct > r.frames / 100) {
                ESP_LOGW(kTag, "VERDICT: the microphone IS alive, in the %s slot. %s",
                         o.what, o.means);
                start_on(I2S_STD_SLOT_LEFT);
                return;
            }
        }
        // Some INMP441 modules will not start at the 1.024 MHz bit clock that
        // 16 kHz gives. If a faster clock wakes it up, that is a rate problem
        // with a known fix (capture higher and downsample), not a dead part.
        ESP_LOGI(kTag, "still nothing. Trying faster bit clocks, in case this module will not "
                       "run at %lu kHz.", static_cast<unsigned long>(kSampleRate * 64 / 1000));
        for (uint32_t rate : {32000u, 48000u}) {
            if (!start_on(I2S_STD_SLOT_LEFT, rate)) continue;
            const Reading r = read(rate / 2);
            ESP_LOGI(kTag, "  %lu Hz (BCLK %lu kHz): distinct %u/%u, peak %d",
                     static_cast<unsigned long>(rate),
                     static_cast<unsigned long>(rate * 64 / 1000),
                     static_cast<unsigned>(r.distinct), static_cast<unsigned>(r.frames), r.peak);
            if (r.ok && r.distinct > r.frames / 100) {
                ESP_LOGW(kTag, "VERDICT: the microphone IS alive, but only at %lu Hz. It will "
                               "not run at the 1.024 MHz bit clock 16 kHz gives. Capture at "
                               "this rate and downsample to 16 kHz for the contract.",
                         static_cast<unsigned long>(rate));
                start_on(I2S_STD_SLOT_LEFT);
                return;
            }
        }
        start_on(I2S_STD_SLOT_LEFT);
        ESP_LOGE(kTag, "VERDICT: the data line never changed in any slot configuration "
                       "or at any bit clock, so this is not the firmware. Either nothing is wired "
                       "to IO%d, or: VDD is not "
                       "3.3 V at the part, SD does not reach IO%d, BCLK does not reach IO%d, or "
                       "L/R is floating instead of tied to ground.",
                 kMicData, kMicData, kMicBclk);
    }
    else if (loudest < 64)
        ESP_LOGW(kTag, "VERDICT: data is arriving but it never got loud (peak %d). The part is "
                       "clocking, so this is the microphone itself or a cold joint on SD. "
                       "Reflow with flux and run it again.", loudest);
    else
        ESP_LOGI(kTag, "VERDICT: the microphone works. Peak %d, and the level moved. "
                       "Next is capture into the ring buffer, not more probing.", loudest);
}

void loopback_check() {
    stop();

    // WS on IO21 and the bit clock on IO17: the opposite of normal, so the
    // jumper already between IO21 and IO19 carries the word select.
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    if (i2s_new_channel(&chan, nullptr, &g_rx) != ESP_OK) {
        ESP_LOGE(kTag, "no free I2S channel");
        return;
    }
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = kMicWs,      // swapped on purpose
            .ws = kMicBclk,      // swapped on purpose
            .dout = I2S_GPIO_UNUSED,
            .din = kMicData,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    esp_err_t err = i2s_channel_init_std_mode(g_rx, &cfg);
    if (err == ESP_OK) err = i2s_channel_enable(g_rx);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "loopback I2S would not start: %s", esp_err_to_name(err));
        i2s_del_channel(g_rx);
        g_rx = nullptr;
        return;
    }
    g_slot = I2S_STD_SLOT_BOTH;
    g_rate = kSampleRate;
    ESP_LOGI(kTag, "loopback: word select out of IO%d, read back on IO%d. Expecting it to "
                   "alternate.", kMicBclk, kMicData);

    const Reading r = read(kSampleRate / 4);
    ESP_LOGI(kTag, "  distinct %u/%u, raw %ld..%ld", static_cast<unsigned>(r.distinct),
             static_cast<unsigned>(r.frames), static_cast<long>(r.raw_min),
             static_cast<long>(r.raw_max));
    if (r.ok && r.distinct > r.frames / 10 && r.raw_min != r.raw_max)
        ESP_LOGW(kTag, "RESULT: the I2S framing is running and IO%d reads what IO%d drives. "
                       "The board, those two header pins and this firmware are all fine, so a "
                       "silent microphone is the module, its joints, or its power.",
                 kMicData, kMicBclk);
    else
        ESP_LOGE(kTag, "RESULT: IO%d did not follow IO%d. Either the jumper is not between "
                       "those two header pins, or they are not the pins you think they are: "
                       "press N and find them with a meter.", kMicData, kMicBclk);

    stop();
    start_on(I2S_STD_SLOT_LEFT);
}

// ---------------------------------------------------------------- capture

namespace {
TaskHandle_t g_capture_task = nullptr;
SampleSink g_sink = nullptr;
volatile bool g_capturing = false;

// A single pole DC blocker, and one bit of headroom.
//
// Both come from the first real recording (2026-10-27, 3.79 s of speech): RMS
// was -10.9 dBFS, about 10 dB hotter than a speech recording wants, with 26
// samples already clipped and a DC offset of 326. Taking the top 16 bits of the
// microphone's 24 is full gain, so a louder voice clips properly, and clipping
// is one of the few things that really costs transcription accuracy. A bit of
// headroom is cheaper than a clipped consonant, and ADPCM should not spend its
// precision tracking a constant.
int32_t g_dc_x1 = 0, g_dc_y1 = 0;

int16_t condition(int32_t raw) {
    const int32_t x = raw >> 17;             // 24 bit sample, one bit of headroom
    // y[n] = x[n] - x[n-1] + 0.995 * y[n-1], at 16 kHz: about 13 Hz corner.
    const int32_t y = x - g_dc_x1 + ((g_dc_y1 * 4079) >> 12);
    g_dc_x1 = x;
    g_dc_y1 = y;
    return static_cast<int16_t>(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
}

void capture_task(void*) {
    static int32_t raw[kBlockFrames];
    static int16_t pcm[kBlockFrames];
    size_t total = 0;
    // The first samples after the receiver is enabled are a startup transient,
    // not sound: measured full scale on the probe (-4.9 dBFS, peak 32753) while
    // the room was quiet, settling about 50 ms later. Recording it would start
    // every command with a pop and spend the first ADPCM chunk chasing it.
    size_t skip = kSampleRate / 20;
    g_dc_x1 = g_dc_y1 = 0;   // the filter starts fresh for every recording
    while (g_capturing) {
        size_t got = 0;
        // A timeout rather than forever, so stopping is never stuck in a read.
        if (i2s_channel_read(g_rx, raw, sizeof(raw), &got, pdMS_TO_TICKS(100)) != ESP_OK)
            continue;
        size_t n = got / sizeof(int32_t);
        const int32_t* from = raw;
        if (skip > 0) {
            const size_t drop = skip < n ? skip : n;
            skip -= drop;
            from += drop;
            n -= drop;
        }
        for (size_t i = 0; i < n; ++i) pcm[i] = condition(from[i]);
        if (n > 0 && g_sink != nullptr) g_sink(pcm, n);
        total += n;
    }
    ESP_LOGI(kTag, "capture ended, %u samples (%.2f s)", static_cast<unsigned>(total),
             static_cast<double>(total) / kSampleRate);
    g_capture_task = nullptr;
    vTaskDelete(nullptr);
}
}  // namespace

bool capturing() { return g_capturing; }

bool capture_start(SampleSink sink) {
    if (g_capturing) return true;
    if (!start_on(I2S_STD_SLOT_LEFT)) return false;
    g_sink = sink;
    g_capturing = true;
    // Above the main loop's priority: a sample missed is speech lost, and the
    // main loop can wait a few microseconds for a screen.
    if (xTaskCreate(capture_task, "halo_mic", 4096, nullptr, 6, &g_capture_task) != pdPASS) {
        g_capturing = false;
        ESP_LOGE(kTag, "could not start the capture task");
        return false;
    }
    ESP_LOGI(kTag, "capture started");
    return true;
}

void capture_stop() {
    if (!g_capturing) return;
    g_capturing = false;
    // The task clears its own handle as it exits; give it the one read timeout.
    for (int i = 0; i < 20 && g_capture_task != nullptr; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

void line_check() {
    stop();   // I2S owns these pins while it is running

    struct Named { const char* what; gpio_num_t pin; };
    const Named pins[] = {
        {"BCLK/SCK  IO21", kMicBclk},
        {"WS/LRCL   IO17", kMicWs},
        {"SD (data) IO19", kMicData},
    };

    ESP_LOGI(kTag, "line check. 'follows the pull' means nothing out there is driving the pin:");
    ESP_LOGI(kTag, "  an unconnected pin, or a connected pin whose other end is not powered.");
    for (const Named& n : pins) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << n.pin;
        cfg.mode = GPIO_MODE_INPUT;

        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&cfg);
        vTaskDelay(pdMS_TO_TICKS(20));
        const int up = gpio_get_level(n.pin);

        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
        gpio_config(&cfg);
        vTaskDelay(pdMS_TO_TICKS(20));
        const int down = gpio_get_level(n.pin);

        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&cfg);

        if (up == 1 && down == 0)
            ESP_LOGW(kTag, "  %s: follows the pull (floating)", n.what);
        else
            ESP_LOGI(kTag, "  %s: held %s by something out there", n.what, up ? "HIGH" : "LOW");
    }
    // Is the bench jumper still in? Drive the clock pin by hand and watch the
    // data pin. If it follows, those two are wired together and the microphone
    // is not what is being read.
    {
        gpio_config_t drive = {};
        drive.pin_bit_mask = 1ULL << kMicBclk;
        drive.mode = GPIO_MODE_OUTPUT;
        gpio_config(&drive);
        gpio_config_t sense = {};
        sense.pin_bit_mask = 1ULL << kMicData;
        sense.mode = GPIO_MODE_INPUT;
        gpio_config(&sense);

        gpio_set_level(kMicBclk, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        const int low = gpio_get_level(kMicData);
        gpio_set_level(kMicBclk, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        const int high = gpio_get_level(kMicData);
        gpio_config(&sense);   // release the clock pin back to an input
        gpio_config_t release = {};
        release.pin_bit_mask = 1ULL << kMicBclk;
        release.mode = GPIO_MODE_INPUT;
        gpio_config(&release);

        if (low == 0 && high == 1)
            ESP_LOGW(kTag, "  IO%d follows IO%d exactly: the bench loopback jumper is STILL IN. "
                           "Pull it and put the microphone's SD back on IO%d.",
                     kMicData, kMicBclk, kMicData);
        else if (low == 1 && high == 1)
            ESP_LOGW(kTag, "  IO%d stays HIGH even with IO%d driven low: something out there is "
                           "holding the data line high. That is the module, not the jumper.",
                     kMicData, kMicBclk);
        else
            ESP_LOGI(kTag, "  IO%d does not follow IO%d (low=%d high=%d): no jumper between them.",
                     kMicData, kMicBclk, low, high);
    }

    ESP_LOGI(kTag, "If SD follows the pull, the microphone is either not connected to IO%d or "
                   "not powered. Measure 3.3 V on the module's own VDD pin next.", kMicData);
    ESP_LOGI(kTag, "Loopback, if you want the board half proved: unplug SD from the microphone "
                   "and jumper IO%d straight to IO%d, then press M. The clock arrives as data "
                   "and the level jumps. If that works, the board and this firmware are fine "
                   "and the fault is the module or its joints.", kMicBclk, kMicData);
}

void identify_pins() {
    stop();   // I2S owns these pins while it is running

    struct Named { const char* what; gpio_num_t pin; };
    const Named pins[] = {
        {"BCLK / SCK", kMicBclk},
        {"WS / LRCL", kMicWs},
        {"SD (the microphone's output)", kMicData},
    };

    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << kMicBclk) | (1ULL << kMicWs) | (1ULL << kMicData);
    out.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&out) != ESP_OK) {
        ESP_LOGE(kTag, "could not drive the microphone pins");
        return;
    }
    for (const Named& n : pins) gpio_set_level(n.pin, 0);

    ESP_LOGI(kTag, "Put the black probe on a GND pin and sweep the 2x10 header with the red "
                   "one. Exactly one pin reads 3.3 V at a time; that is the one named below.");
    for (const Named& n : pins) {
        gpio_set_level(n.pin, 1);
        ESP_LOGI(kTag, "  >>> IO%d is HIGH now: this is %s <<<", n.pin, n.what);
        vTaskDelay(pdMS_TO_TICKS(6000));
        gpio_set_level(n.pin, 0);
    }

    // Back to inputs, so nothing is left driving a pin the microphone will own.
    gpio_config_t in = {};
    in.pin_bit_mask = out.pin_bit_mask;
    in.mode = GPIO_MODE_INPUT;
    gpio_config(&in);
    ESP_LOGI(kTag, "done; pins released. The 3.3 V and GND pins on that header are the ones "
                   "that read 3.3 V and 0 V the whole time.");
}

}  // namespace halo::board::mic
