// F0 on real hardware: read the buttons and the knob, turn them into V1 gestures,
// run the state machine, and log every input and every transition over serial.
//
// No BLE and no screen rendering yet. Everything the phone would send is injected
// over serial instead (type a letter in idf.py monitor), so all 21 states can be
// walked on the bench before any radio exists.
#include <cstdio>

#include "ble.hpp"
#include "board_pins.hpp"
#include "display_backends.hpp"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "halo/audio.hpp"
#include "halo/gestures.hpp"
#include "halo/json.hpp"
#include "halo/layout.hpp"
#include "halo/state_machine.hpp"

using namespace halo;

namespace {

namespace ble = halo::board::ble;

constexpr const char* kTag = "halo";
constexpr TickType_t kPollMs = 5;  // the gesture detector needs ticks, not just edges

Config g_cfg;
GestureDetector g_gestures(g_cfg);
StateMachine g_machine(g_cfg);
RefreshPolicy g_refresh;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// All buttons are active low. The onboard ones have external 4.7 K pull ups on
// the board, so they need no internal pull. Anything on the breadboard header
// does: without one the pin floats, reads as pressed at random, and the device
// starts recording on its own. That is exactly what happened on first boot.
void configure_input(gpio_num_t pin, bool internal_pullup) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = internal_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

bool pressed(gpio_num_t pin) { return gpio_get_level(pin) == 0; }

void log_transition(const Transition& t, const Event& e) {
    if (!t.handled) {
        ESP_LOGD(kTag, "%8lu  %-22s ignored in %s", static_cast<unsigned long>(e.at_ms),
                 name(e.kind), name(t.from));
        return;
    }
    if (!t.changed()) {
        ESP_LOGI(kTag, "%8lu  %-22s %s (%s)", static_cast<unsigned long>(e.at_ms), name(e.kind),
                 name(t.from), t.note);
        return;
    }
    char actions[96] = "";
    size_t at = 0;
    for (uint8_t i = 0; i < t.actions.count && at < sizeof(actions) - 1; ++i)
        at += snprintf(actions + at, sizeof(actions) - at, "%s%s", at ? ", " : "", name(t.actions.items[i]));
    ESP_LOGI(kTag, "%8lu  %-22s %s -> %s  [%s]  %s", static_cast<unsigned long>(e.at_ms),
             name(e.kind), name(t.from), name(t.to), actions, t.note);
}

void feed(const Event& e) {
    const Transition t = g_machine.handle(e);
    log_transition(t, e);

    // The only screen work F0 does: prove the layout and the present callback are
    // wired, so the REVIEW confirm guard starts when the panel says it is drawn.
    if (t.changed() && (t.actions.has(Action::DrawScreen) || t.actions.has(Action::ShowWallpaper))) {
        // Static, not on the stack: a DisplayList is close to a kilobyte and the
        // main task does not have that to spare. Single threaded, so sharing one
        // is safe, and it makes the memory cost visible at link time.
        static DisplayList list;
        ScreenModel m;
        m.state = t.to;
        m.status_line = name(t.to);
        layout_screen(board::display(), m, list);
        board::display().draw(list);
        const Refresh intent = g_refresh.choose(t.to, false);
        board::display().present(intent, [](void*, uint32_t at) {
            const Transition pt = g_machine.handle(Event::at(EventKind::ScreenPresented, at));
            log_transition(pt, Event::at(EventKind::ScreenPresented, at));
        }, nullptr);
        g_refresh.note_presented(intent);
    }
}

// ---------------------------------------------------------------- audio out
//
// Streams a segment as 176 byte chunks, respecting the phone's cumulative
// acknowledgments, and resends from `resend_from` when it reports a gap.
//
// The source here is a clip embedded in flash rather than a microphone, because
// this board has no microphone. Everything downstream of the samples is the real
// path: the real encoder, the real byte layout, the real ack protocol, the real
// radio. What it cannot test is capture itself, which waits for the badge.
extern const uint8_t kClipStart[] asm("_binary_test_clip_pcm_start");
extern const uint8_t kClipEnd[] asm("_binary_test_clip_pcm_end");

class AudioTx {
public:
    // Stay this many chunks ahead of the acknowledgment at most. On the real
    // device the limit is the 2 s ring buffer; here it is just flow control.
    static constexpr int kWindow = 24;
    static constexpr int kMaxChunks = 400;  // 8 s at 20 ms per chunk

    void start(uint16_t req, uint8_t rev, uint8_t seg, const int16_t* pcm, size_t samples) {
        req_ = req; rev_ = rev; seg_ = seg;
        pcm_ = pcm; samples_ = samples;
        total_chunks_ = static_cast<int>((samples + kSamplesPerChunk - 1) / kSamplesPerChunk);
        if (total_chunks_ > kMaxChunks) total_chunks_ = kMaxChunks;
        next_ = 0; acked_ = -1; final_sent_ = false; done_ = false;
        started_us_ = esp_timer_get_time();
        bytes_ = 0; resends_ = 0;
        // Record the encoder state at the start of every chunk so a resend does
        // not have to replay the whole segment.
        AdpcmEncoder e;
        static int16_t part[kSamplesPerChunk];
        static uint8_t scratch[kChunkPayloadBytes];
        for (int i = 0; i < total_chunks_; ++i) {
            state_[i] = e.state();
            const size_t n = fill(i, part);
            e.encode(part, n, scratch);
        }
        ESP_LOGI(kTag, "audio: %u samples, %d chunks, %.1f s",
                 static_cast<unsigned>(samples), total_chunks_, samples / 16000.0);
    }

    bool active() const { return pcm_ != nullptr && !done_; }

    void on_ack(int32_t upto, bool has_resend, int32_t resend_from, bool final_ack) {
        if (final_ack) {
            const int64_t ms = (esp_timer_get_time() - started_us_) / 1000;
            const double kbps = ms > 0 ? (bytes_ * 8.0) / ms : 0;
            ESP_LOGI(kTag, "audio ACKED in %lld ms, %u bytes, %.0f kbit/s, %d resends, "
                           "%.1fx realtime",
                     ms, static_cast<unsigned>(bytes_), kbps, resends_,
                     (samples_ / 16000.0) * 1000.0 / (ms ? ms : 1));
            done_ = true;
            return;
        }
        if (upto > acked_) acked_ = upto;
        if (has_resend && resend_from >= 0 && resend_from < total_chunks_) {
            next_ = resend_from;
            ++resends_;
        }
    }

    // Called from the main loop. Sends while the window allows.
    void poll() {
        if (!active() || !ble::connected()) return;
        static uint8_t chunk[kChunkBytes];
        static int16_t part[kSamplesPerChunk];
        static uint8_t payload[kChunkPayloadBytes];
        while (next_ < total_chunks_ && (next_ - acked_) <= kWindow) {
            const size_t n = fill(next_, part);
            AdpcmEncoder e;
            e.set_state(state_[next_]);
            e.encode(part, n, payload);
            const size_t len = build_chunk(chunk, req_, rev_, seg_, static_cast<uint16_t>(next_),
                                           payload, static_cast<uint16_t>(n / 2), state_[next_]);
            if (!ble::notify_audio(chunk, len)) return;  // buffers full, try again next poll
            bytes_ += len;
            ++next_;
        }
        if (next_ >= total_chunks_ && !final_sent_) {
            const size_t len = build_final_marker(chunk, req_, rev_, seg_,
                                                  static_cast<uint16_t>(total_chunks_),
                                                  static_cast<uint32_t>(samples_));
            if (ble::notify_audio(chunk, len)) {
                final_sent_ = true;
                bytes_ += len;
                ESP_LOGI(kTag, "audio: final marker sent, waiting for its ack");
            }
        }
    }

private:
    size_t fill(int seq, int16_t* out) const {
        const size_t start = static_cast<size_t>(seq) * kSamplesPerChunk;
        size_t n = 0;
        while (n < kSamplesPerChunk && start + n < samples_) out[n] = pcm_[start + n], ++n;
        if (n % 2) out[n++] = 0;  // pad to an even sample count
        return n;
    }

    uint16_t req_ = 0; uint8_t rev_ = 0, seg_ = 0;
    const int16_t* pcm_ = nullptr;
    size_t samples_ = 0;
    int total_chunks_ = 0, next_ = 0, acked_ = -1, resends_ = 0;
    bool final_sent_ = false, done_ = true;
    int64_t started_us_ = 0;
    size_t bytes_ = 0;
    AdpcmState state_[kMaxChunks];
};

AudioTx g_audio;

void send_event(const char* type, int32_t a = -1, const char* akey = nullptr) {
    char buf[256];
    json::Writer w(buf, sizeof(buf));
    w.key_str("type", type);
    if (akey) w.key_int(akey, a);
    const char* out = w.finish();
    if (out) ble::notify_event(out, w.length());
}

void on_audio_ack(const char* js, size_t len) {
    int32_t upto = -1, resend = -1;
    bool fin = false;
    json::get_int(js, len, "upto", &upto);
    const bool has_resend = json::get_int(js, len, "resend_from", &resend);
    json::get_bool(js, len, "final", &fin);
    g_audio.on_ack(upto, has_resend, resend, fin);
}

void on_events_subscribed() {
    // hello goes out here, not on connect: a notification sent before the phone
    // subscribes is dropped and nobody ever sees it.
    char buf[256];
    json::Writer w(buf, sizeof(buf));
    w.key_str("type", "hello");
    w.key_str("fw", "f0-dev");
    w.key_int("boot", static_cast<int32_t>(esp_timer_get_time() & 0x7fffffff));
    w.key_str("state", name(g_machine.state()));
    w.key_int("req", 1);
    w.key_null("sid");
    w.key_null("pending_key");
    w.key_int("battery", 100);
    const char* out = w.finish();
    if (out) {
        ble::notify_event(out, w.length());
        ESP_LOGI(kTag, "hello sent, MTU %u", ble::mtu());
    }
}

void on_settings(const char* js, size_t len) {
    bool ready = false;
    if (json::get_bool(js, len, "phone_ready", &ready))
        ESP_LOGI(kTag, "settings: phone_ready=%d", ready ? 1 : 0);
}

// Everything the phone will eventually send, injected by a keystroke so the whole
// state machine can be walked on the bench with no radio.
// Read the console through the UART driver rather than getchar(). The default
// console VFS ignores O_NONBLOCK, so getchar() blocks forever, the poll loop
// never reaches its vTaskDelay, and the task watchdog kills the idle task. Found
// on real hardware; it cannot happen in the host tests because there is no loop.
void handle_serial() {
    uint8_t byte = 0;
    if (uart_read_bytes(UART_NUM_0, &byte, 1, 0) != 1) return;
    switch (byte) {
        case 'p': feed(Event::at(EventKind::PhoneReady, now_ms())); break;
        case 'P': feed(Event::at(EventKind::PhoneAbsent, now_ms())); break;
        case 'a': feed(Event::at(EventKind::AudioComplete, now_ms())); break;
        case 'r': feed(Event::at(EventKind::ScreenReview, now_ms())); break;
        case 'c': feed(Event::at(EventKind::ScreenConflictAmbiguous, now_ms())); break;
        case 's': feed(Event::at(EventKind::ScreenConflictSchedule, now_ms())); break;
        case 'o': feed(Event::at(EventKind::ScreenReadOnly, now_ms())); break;
        case 'k': feed(Event::at(EventKind::ExecutionApplied, now_ms())); break;
        case 'f': feed(Event::at(EventKind::ExecutionRejected, now_ms())); break;
        case 'u': feed(Event::at(EventKind::ExecutionUnknown, now_ms())); break;
        case 'x': feed(Event::at(EventKind::TransportFailure, now_ms())); break;
        case 'b': feed(Event::at(EventKind::BatteryLevel, now_ms(), 4)); break;   // critical
        case 'B': feed(Event::at(EventKind::BatteryLevel, now_ms(), 80)); break;  // recovered
        // Gesture injection, bench only. These bypass the gesture detector, NOT the
        // state machine: the 1 s confirm guard and every other rule still apply,
        // because they live in the state machine and nothing here can reach past
        // it. Compiled out unless HALO_BENCH_INJECT is defined, so it cannot ship.
#ifdef HALO_BENCH_INJECT
        case 'h': feed(Event::at(EventKind::HoldStart, now_ms())); break;
        case 'R': feed(Event::at(EventKind::Release, now_ms())); break;
        case '1': feed(Event::at(EventKind::SinglePress, now_ms())); break;
        case '2': feed(Event::at(EventKind::DoublePress, now_ms())); break;
        case '<': feed(Event::at(EventKind::RotateDown, now_ms(), -1)); break;
        case '>': feed(Event::at(EventKind::RotateUp, now_ms(), 1)); break;
#endif
        case 'A':  // stream the embedded clip as if it had just been recorded
            if (ble::connected()) {
                const int16_t* pcm = reinterpret_cast<const int16_t*>(kClipStart);
                const size_t samples = static_cast<size_t>(kClipEnd - kClipStart) / 2;
                send_event("record_start");
                g_audio.start(513, 2, 3, pcm, samples);
            } else {
                ESP_LOGW(kTag, "no phone connected");
            }
            break;
        case 'C':
            ESP_LOGI(kTag, "deep cleaning the panel, a few seconds ...");
            board::deep_clean(4);
            ESP_LOGI(kTag, "panel cleaned");
            break;
        case 'T': {
            // Orientation probe. I cannot see the panel, so this draws something
            // whose description settles both questions at once: if line 1 is at
            // the bottom the image is flipped vertically, and if the text reads
            // backwards it is mirrored horizontally.
            static DisplayList probe;
            probe.clear();
            probe.rect(RectCmd{Box{0, 0, 40, 40}, true, 0});
            static const char* const kLines[] = {
                "1 TOP-LEFT >>>>>",
                "2 the quick brown fox",
                "3 HALOAI 0123456789",
                "4",
                "5 BOTTOM-LAST-LINE",
            };
            const TextMetrics tm = board::display().metrics(TextStyle::Body);
            for (int i = 0; i < 5; ++i) {
                TextCmd t;
                t.box = Box{48, static_cast<int16_t>(56 + i * tm.line_h), 700, tm.line_h};
                t.text = kLines[i];
                probe.text(t);
            }
            board::display().draw(probe);
            board::display().present(Refresh::Full, nullptr, nullptr);
            ESP_LOGI(kTag, "test pattern drawn: tell me where line 1 is and "
                           "whether the text reads normally");
            break;
        }
        case '?':
            ESP_LOGI(kTag, "p/P phone ready|absent, a audio acked, r/c/s/o results, "
                           "k/f/u execution, x transport fail, b/B battery");
            break;
        default: break;
    }
}


struct Edge {
    gpio_num_t pin;
    bool was_down = false;
    uint32_t last_change = 0;
};

// The gesture detector debounces the side button. The knob had no debounce at
// all, and the first hardware run showed the same press arriving three times
// about 80 ms apart.
bool debounced_edge(Edge& e, uint32_t t, uint32_t debounce_ms) {
    const bool down_now = pressed(e.pin);
    if (down_now == e.was_down) return false;
    if (e.last_change != 0 && t - e.last_change < debounce_ms) return false;
    e.was_down = down_now;
    e.last_change = t;
    return down_now;  // true only on the press edge
}

}  // namespace

extern "C" void app_main() {
    esp_log_level_set(kTag, ESP_LOG_DEBUG);
    ESP_LOGI(kTag, "HaloAI device, F0 build. Panel: %s", board::panel_name());
    ESP_LOGI(kTag, "hold MENU to record, press to confirm, double press to cancel, knob to scroll");
    ESP_LOGI(kTag, "press ? in the monitor for the keys that inject phone events");
#ifdef HALO_BENCH_INJECT
    ESP_LOGW(kTag, "BENCH BUILD: gestures can be injected over serial (h R 1 2 < >). "
                   "The confirm guard still applies. Do not ship this build.");
#endif

    for (gpio_num_t p : {board::kBtnMenu, board::kBtnExit, board::kKnobUp, board::kKnobDown,
                         board::kKnobPress})
        configure_input(p, false);          // onboard: external 4.7 K pull ups
    configure_input(board::kBtnSide, true);  // breadboard: needs the internal one

    // Non blocking console reads, see handle_serial().
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0));

    board::display().init();

    // NVS is where the pending execution key will live in F3; NimBLE needs it too.
    if (nvs_flash_init() == ESP_ERR_NVS_NO_FREE_PAGES) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ble::Handlers h;
    h.on_audio_ack = on_audio_ack;
    h.on_settings = on_settings;
    h.on_events_subscribed = on_events_subscribed;
    ble::start("HaloAI", h);

    // Startup checks. F3 replaces this with a real read of the pending execution
    // key from flash, which is what sends BOOT / RECOVERY to RECONCILE STATUS.
    feed(Event::at(EventKind::StartupOk, now_ms()));

    Edge menu{board::kBtnMenu};
    Edge side{board::kBtnSide};
    Edge up{board::kKnobUp};
    Edge down{board::kKnobDown};
    Edge knob{board::kKnobPress};
    Edge exitb{board::kBtnExit};

    for (;;) {
        const uint32_t t = now_ms();

        // The side button and MENU both drive the gesture detector: MENU is what
        // is on this board, the external side button is the part the badge ships.
        const bool button_down = pressed(menu.pin) || pressed(side.pin);
        const bool was_down = menu.was_down || side.was_down;
        if (button_down != was_down) {
            if (button_down) g_gestures.button_down(t);
            else g_gestures.button_up(t);
            menu.was_down = side.was_down = button_down;
        }
        g_gestures.tick(t);

        Event gesture;
        while (g_gestures.next(gesture)) feed(gesture);

        // Knob: one event per debounced press edge, not per poll.
        for (Edge* e : {&up, &down, &knob}) {
            if (!debounced_edge(*e, t, g_cfg.debounce_ms)) continue;
            if (e->pin == board::kKnobUp) feed(Event::at(EventKind::RotateUp, t, 1));
            else if (e->pin == board::kKnobDown) feed(Event::at(EventKind::RotateDown, t, -1));
            else feed(Event::at(EventKind::Wake, t));
        }

        if (debounced_edge(exitb, t, g_cfg.debounce_ms))
            ESP_LOGI(kTag, "%8lu  EXIT button (unmapped)", static_cast<unsigned long>(t));

#ifdef HALO_BENCH_INJECT
        // Raw pin watch, bench only. Says which physical pin moved, so a button
        // that does nothing can be told apart from a button on the wrong pin.
        static uint32_t last_raw = 0xffffffff;
        const uint32_t raw = (pressed(board::kBtnMenu) ? 1u : 0) | (pressed(board::kBtnExit) ? 2u : 0) |
                             (pressed(board::kKnobUp) ? 4u : 0) | (pressed(board::kKnobDown) ? 8u : 0) |
                             (pressed(board::kKnobPress) ? 16u : 0) | (pressed(board::kBtnSide) ? 32u : 0);
        if (raw != last_raw) {
            ESP_LOGI(kTag, "%8lu  RAW menu=%d exit=%d up=%d down=%d knob=%d side=%d",
                     static_cast<unsigned long>(t), (raw & 1) ? 1 : 0, (raw & 2) ? 1 : 0,
                     (raw & 4) ? 1 : 0, (raw & 8) ? 1 : 0, (raw & 16) ? 1 : 0, (raw & 32) ? 1 : 0);
            last_raw = raw;
        }
#endif

        g_audio.poll();
        g_machine.tick(t);
        handle_serial();
        // Never let this round to zero ticks: that starves the idle task and
        // trips the watchdog. See CONFIG_FREERTOS_HZ in sdkconfig.defaults.
        constexpr TickType_t kDelay = pdMS_TO_TICKS(kPollMs) > 0 ? pdMS_TO_TICKS(kPollMs) : 1;
        vTaskDelay(kDelay);
    }
}
