// The device app: read the buttons and the knob, turn them into V1 gestures, run
// the state machine, draw what the phone sends, and speak the agreed BLE contract.
//
// This file is glue and nothing else. Every decision worth testing lives in core/
// (the state machine, the gestures, the layout, the audio codec, the messages) and
// is covered by host tests; what is left here is pins, tasks and the radio.
//
// Anything the phone would send can still be injected over serial (type a letter
// in idf.py monitor), so all 21 states can be walked with no phone in the room.
#include <cstdio>

#include "ble.hpp"
#include "wallpaper_store.hpp"
#include "board_pins.hpp"
#include "display_backends.hpp"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "halo/audio.hpp"
#include "halo/gestures.hpp"
#include "halo/json.hpp"
#include "halo/layout.hpp"
#include "halo/messages.hpp"
#include "halo/screens.hpp"
#include "halo/state_machine.hpp"

using namespace halo;

namespace {

namespace ble = halo::board::ble;

constexpr const char* kTag = "halo";
// Reported in `hello`, so the phone can tell which firmware it is talking to.
constexpr const char* kFirmwareVersion = "f2-dev";
constexpr TickType_t kPollMs = 5;  // the gesture detector needs ticks, not just edges

Config g_cfg;
GestureDetector g_gestures(g_cfg);
StateMachine g_machine(g_cfg);
RefreshPolicy g_refresh;

// Everything the phone is told about. The link carries the IDs every event
// echoes; the screen is the last one the phone sent, kept because the knob can
// scroll it long after the write arrived.
msg::Link g_link;
msg::Screen g_screen;
msg::Settings g_settings;
int8_t g_highlight = -1;    // candidate under the bar in CONFLICT-AMBIGUOUS
uint8_t g_scroll = 0;       // first body line drawn
uint8_t g_overflow = 0;     // body lines that did not fit, from layout_screen
uint32_t g_boot = 0;        // random per power up: tells a reconnect from a restart
uint8_t g_battery_pct = 100;
uint8_t g_battery_sent = 255;

// The wallpaper the user picked on the phone. The store owns the flash; the
// receiver owns every decision about what to accept, and is tested on a laptop.
board::WallpaperStore g_wallpaper;
wp::Receiver g_wallpaper_rx(g_wallpaper);
// Counted on the radio task. Against the bytes the receiver accepted, it says
// whether a missing chunk was lost on the air or lost inside this firmware.
volatile uint32_t g_wallpaper_writes = 0;

// Everything from the phone arrives on the NimBLE host task. The state machine,
// the gesture detector and the display all live on the main task and not one of
// them is thread safe, so nothing from the radio touches them directly: the
// callbacks copy the payload into this queue and the main loop drains it.
//
// A dropped message is logged, never silent. The screen the phone sent is the
// source of truth for every decision state, so losing one is a real failure and
// has to be visible, not a gap in a log that reads as if nothing happened.
enum class Incoming : uint8_t { Screen, Settings, Wallpaper, EventsSubscribed, Disconnected };

struct Message {
    Incoming kind = Incoming::Screen;
    uint16_t len = 0;
    char data[1024] = {};  // a Screen write is the largest thing the phone sends
};

QueueHandle_t g_inbox = nullptr;

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
        // This was ESP_LOGD, and CONFIG_LOG_MAXIMUM_LEVEL is INFO, so the macro
        // was compiled out and esp_log_level_set() could not bring it back: every
        // event that did nothing left no trace at all. Found by Carlos on
        // 2026-10-20, after sending several screens that were silently ignored.
        // Tick and ScreenPresented are excluded: both are housekeeping the
        // device sends itself, constantly and by design, and neither says
        // anything about why a press or a screen did nothing.
        if (e.kind != EventKind::Tick && e.kind != EventKind::ScreenPresented)
            ESP_LOGI(kTag, "%8lu  %-22s ignored in %s", static_cast<unsigned long>(e.at_ms),
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

// Draws the state the device is in. When the phone has sent a screen for that
// state, the phone's text wins: the backend owns the words, the device owns the
// pixels. `status_only` is the partial refresh path, about 495 ms on this panel
// against 4.3 s for a full clear and redraw.
void draw_screen(State to, bool status_only) {
    // Static, not on the stack: a DisplayList is close to a kilobyte and the
    // main task does not have that to spare. Single threaded, so sharing one
    // is safe, and it makes the memory cost visible at link time.
    static DisplayList list;
    ScreenModel m;
    m.state = to;

    // Nine of the 21 states are the device's own and nothing sends text for them.
    // The other twelve belong to the backend, and their words arrive over BLE.
    DeviceStatus st;
    st.battery_pct = g_battery_pct;
    st.phone_linked = ble::connected() && g_settings.phone_ready;
    st.pending_execution = g_machine.has_pending_execution();
    // Null unless an image is stored for the orientation that is set now, in
    // which case WALLPAPER says the device's name instead of showing a sideways
    // photo, and the phone can send a new one.
    st.wallpaper = g_wallpaper.current(g_settings.portrait);
    const bool ours = local_screen(to, st, m);

    if (!ours) m.status_line = name(to);

    if (!ours && g_screen.has_state && g_screen.state == to) {
        if (g_screen.has_status) m.status_line = g_screen.status_line;
        if (g_screen.line_count > 0) {
            m.lines = g_screen.line_ptrs;
            m.line_count = g_screen.line_count;
        }
        if (g_screen.candidate_count > 0) {
            m.candidates = g_screen.candidate_ptrs;
            m.candidate_count = g_screen.candidate_count;
            m.highlight = g_highlight;
        }
        m.expires_in_s = g_screen.expires_in_s;
        m.scroll_line = g_scroll;
    }

    g_overflow = layout_screen(board::display(), m, list);
    board::display().draw(list);
    const Refresh intent = g_refresh.choose(to, status_only);
    board::display().present(intent, [](void*, uint32_t at) {
        // The REVIEW confirm guard starts here, when the panel says the pixels
        // are there, never when the screen was queued.
        const Transition pt = g_machine.handle(Event::at(EventKind::ScreenPresented, at));
        log_transition(pt, Event::at(EventKind::ScreenPresented, at));
    }, nullptr);
    g_refresh.note_presented(intent);
}

// Turns one transition into the events the contract says the phone gets. Nothing
// else in the firmware sends an Events message about a state change, so the list
// of what the phone can be told is exactly this function.
void emit_events(const Transition& t, const Event& e);

// Returns the transition, so a caller can tell an ignored event from an acted one.
Transition feed(const Event& e) {
    // The knob moves the highlight and the scroll before the machine sees the
    // event: a select has to carry the candidate that was under the bar.
    if (e.kind == EventKind::RotateUp || e.kind == EventKind::RotateDown) {
        const int delta = e.kind == EventKind::RotateUp ? -1 : 1;
        if (g_machine.state() == State::ConflictAmbiguous && g_screen.candidate_count > 0) {
            int h = (g_highlight < 0 ? 0 : g_highlight) + delta;
            if (h < 0) h = 0;
            if (h >= g_screen.candidate_count) h = g_screen.candidate_count - 1;
            g_highlight = static_cast<int8_t>(h);
        } else if (g_overflow > 0 || g_scroll > 0) {
            int sc = g_scroll + delta;
            if (sc < 0) sc = 0;
            if (sc > g_scroll + g_overflow) sc = g_scroll + g_overflow;
            g_scroll = static_cast<uint8_t>(sc);
        }
    }

    const Transition t = g_machine.handle(e);
    log_transition(t, e);
    emit_events(t, e);

    // PHONE / BLUETOOTH CHECK is answered from what the phone last told us, not
    // by waiting for it to tell us again. Settings arrives once, right after
    // hello, so a hold minutes later used to sit here until the hold was released
    // and the request died as "released early" (found by Carlos, 2026-10-20).
    // Section 2: RECORDING needs the link up AND the last phone_ready true.
    //
    // Answered before the screen is drawn on purpose. A refresh is about half a
    // second, and spending it on a screen we are leaving immediately would delay
    // the microphone by that much, which on the badge is half a second of speech.
    if (t.changed() && t.to == State::PhoneCheck) {
        const bool ready = ble::connected() && g_settings.phone_ready;
        const Transition answered =
            feed(Event::at(ready ? EventKind::PhoneReady : EventKind::PhoneAbsent, e.at_ms));
        if (answered.changed()) return t;  // the nested call drew where we landed
    }

    if (t.changed() && (t.actions.has(Action::DrawScreen) || t.actions.has(Action::ShowWallpaper)))
        draw_screen(t.to, false);
    return t;
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

// Defined below, next to the other message builders. Declared here because the
// streamer is what knows when the last chunk went out and when the link gave up.
void send_record_end(uint16_t req, uint8_t rev, uint8_t seg, uint16_t chunks, uint32_t ms);
void audio_delivery_failed();

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
        record_end_sent_ = false; final_resends_ = 0; final_first_us_ = 0; final_last_us_ = 0;
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
    // Chunks handed to the radio so far. On the badge this is the real count for
    // record_end; here it counts the clip being replayed from flash.
    uint16_t chunks_sent() const { return static_cast<uint16_t>(next_ < 0 ? 0 : next_); }

    void on_ack(int32_t upto, bool has_resend, int32_t resend_from, bool final_ack) {
        if (final_ack) {
            const int64_t ms = (esp_timer_get_time() - started_us_) / 1000;
            const double kbps = ms > 0 ? (bytes_ * 8.0) / ms : 0;
            ESP_LOGI(kTag, "audio ACKED in %lld ms, %u bytes, %.0f kbit/s, %d resends, "
                           "%d final resends, %.1fx realtime",
                     ms, static_cast<unsigned>(bytes_), kbps, resends_, final_resends_,
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
        if (next_ < total_chunks_) return;

        // Every chunk is queued. That is the moment the recording is over, so
        // record_end goes out here: on the badge this is the button release, and
        // it is what lets the phone measure release to complete.
        if (!record_end_sent_) {
            record_end_sent_ = true;
            send_record_end(req_, rev_, seg_, static_cast<uint16_t>(total_chunks_),
                            static_cast<uint32_t>(samples_ / 16));  // 16 samples per ms
        }

        // The final marker is resent until the phone acknowledges it. One lost
        // marker would otherwise hang the run: the phone waits for an end it is
        // never told about, and the device waits for an ack that never comes.
        const int64_t now = esp_timer_get_time();
        const bool due = !final_sent_ ||
                         (now - final_last_us_) / 1000 >= static_cast<int64_t>(g_cfg.final_marker_resend_ms);
        if (due) {
            const size_t len = build_final_marker(chunk, req_, rev_, seg_,
                                                  static_cast<uint16_t>(total_chunks_),
                                                  static_cast<uint32_t>(samples_));
            if (ble::notify_audio(chunk, len)) {
                bytes_ += len;
                final_last_us_ = now;
                if (!final_sent_) {
                    final_sent_ = true;
                    final_first_us_ = now;
                    ESP_LOGI(kTag, "audio: final marker sent, waiting for its ack");
                } else {
                    ++final_resends_;
                    ESP_LOGW(kTag, "audio: final marker not acked, resend %d", final_resends_);
                }
            }
        }

        // Bounded, so a link that has genuinely gone does not resend forever.
        if (final_sent_ &&
            (now - final_first_us_) / 1000 >= static_cast<int64_t>(g_cfg.delivery_deadline_ms)) {
            ESP_LOGE(kTag, "audio: final marker unacknowledged after %lu ms, giving up",
                     static_cast<unsigned long>(g_cfg.delivery_deadline_ms));
            done_ = true;
            audio_delivery_failed();
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
    bool final_sent_ = false, done_ = true, record_end_sent_ = false;
    int final_resends_ = 0;
    int64_t started_us_ = 0, final_first_us_ = 0, final_last_us_ = 0;
    size_t bytes_ = 0;
    AdpcmState state_[kMaxChunks];
};

AudioTx g_audio;

// One buffer for every outgoing event. The build step returns 0 when the message
// did not fit, and 0 means do not send: a truncated object is not JSON.
char g_tx[320];

void send(size_t len, const char* what) {
    if (len == 0) {
        ESP_LOGE(kTag, "event %s did not fit in %u bytes: not sent", what,
                 static_cast<unsigned>(sizeof(g_tx)));
        return;
    }
    if (!ble::notify_event(g_tx, len)) ESP_LOGW(kTag, "event %s not delivered", what);
    else ESP_LOGD(kTag, "-> %.*s", static_cast<int>(len), g_tx);
}

// Why the device cancelled. Only two of the four reasons can arrive here as an
// Action: the V1 table emits SendCancel on a double press and on the 60 s
// expiry. The transport and battery cancels are sent from the transition itself,
// below, because the state machine has no action for them.
msg::CancelReason reason_for(EventKind k) {
    switch (k) {
        case EventKind::DoublePress: return msg::CancelReason::User;
        case EventKind::Tick: return msg::CancelReason::Expired;
        case EventKind::TransportFailure: return msg::CancelReason::Transport;
        case EventKind::BatteryLevel: return msg::CancelReason::Battery;
        default: return msg::CancelReason::User;
    }
}

// Active work that has NOT been dispatched yet. Once approve has gone out, the
// effect may already exist, so the device never reports a cancel for it: EXECUTING
// and everything after it are reconciled by execution key, never cancelled. Saying
// "cancelled" there would be a claim the device cannot make.
bool is_pre_execution(State s) {
    switch (s) {
        case State::Executing:
        case State::ExecutionUncertain:
        case State::ReconcileStatus:
            return false;
        default:
            return is_active_work(s);
    }
}

bool is_input_event(EventKind k) {
    switch (k) {
        case EventKind::SinglePress:
        case EventKind::DoublePress:
        case EventKind::HoldStart:
        case EventKind::Release:
        case EventKind::RotateUp:
        case EventKind::RotateDown:
        case EventKind::Wake:
            return true;
        default:
            return false;
    }
}

void on_audio_ack(const char* js, size_t len) {
    msg::AudioAck a;
    if (!msg::parse_audio_ack(js, len, &a)) {
        ESP_LOGW(kTag, "unreadable audio ack, ignored");
        return;
    }
    g_audio.on_ack(a.upto, a.has_resend, a.resend_from, a.final_segment);
}

// record_end is informational: the final marker in the audio stream, not this, is
// what tells the phone the recording is complete. It carries the chunk count and
// the hold length so the phone can measure release to complete against the budget.
// ---------------------------------------------------- pending execution key
//
// A dispatched action outlives a reboot. The backend owns the execution and its
// status; the device keeps the key so BOOT / RECOVERY, and a return from CRITICAL
// BATTERY, can ask what happened instead of guessing or acting twice.
//
// The key is written as the device LEAVES REVIEW, in the same transition that
// sends approve, so it has to be in hand before the user confirms: it arrives on
// the REVIEW screen, not later.
constexpr const char* kNvsNamespace = "halo";
constexpr const char* kNvsExecutionKey = "exec_key";

void persist_execution_key() {
    if (g_link.pending_key[0] == '\0') {
        // Approving with no key means the phone never sent one. The action still
        // goes out, but nothing could reconcile it after a reboot, so this is
        // loud rather than silent.
        ESP_LOGE(kTag, "approving with no execution key: a reboot could not reconcile this");
        return;
    }
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(kTag, "could not open NVS to save the execution key");
        return;
    }
    const esp_err_t rc = nvs_set_str(h, kNvsExecutionKey, g_link.pending_key);
    if (rc == ESP_OK) nvs_commit(h);
    nvs_close(h);
    if (rc == ESP_OK) ESP_LOGI(kTag, "execution key saved: %s", g_link.pending_key);
    else ESP_LOGE(kTag, "could not save the execution key, rc=%d", rc);
}

void clear_execution_key() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, kNvsExecutionKey);
    nvs_commit(h);
    nvs_close(h);
    g_link.pending_key[0] = '\0';
    ESP_LOGI(kTag, "execution key cleared: the outcome is known");
}

// True when a key was found, which is what sends BOOT / RECOVERY to reconcile.
bool load_execution_key() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(g_link.pending_key);
    const esp_err_t rc = nvs_get_str(h, kNvsExecutionKey, g_link.pending_key, &len);
    nvs_close(h);
    if (rc != ESP_OK) {
        g_link.pending_key[0] = '\0';
        return false;
    }
    return g_link.pending_key[0] != '\0';
}

void send_record_end(uint16_t req, uint8_t rev, uint8_t seg, uint16_t chunks, uint32_t ms) {
    msg::Link l = g_link;
    l.req = req;
    l.rev = rev;
    l.seg = seg;
    send(msg::record_end(g_tx, sizeof(g_tx), l, chunks, ms), "record_end");
}

// The final marker was never acknowledged inside the delivery deadline. Audio is
// never silently dropped: the state machine is told, and FINALIZE AUDIO fails.
void audio_delivery_failed() {
    feed(Event::at(EventKind::TransportFailure, now_ms()));
}

void emit_events(const Transition& t, const Event& e) {
    bool cancel_sent = false;
    for (uint8_t i = 0; i < t.actions.count; ++i) {
        switch (t.actions.items[i]) {
            case Action::SendApprove:
                // The only message in the whole firmware that can change anything
                // in the world. It carries the sid of the screen that was drawn,
                // so the backend binds the approval to exactly what was read.
                send(msg::approve(g_tx, sizeof(g_tx), g_link), "approve");
                break;
            case Action::SendCancel:
                send(msg::cancel(g_tx, sizeof(g_tx), g_link, reason_for(e.kind)), "cancel");
                cancel_sent = true;
                break;
            case Action::SendSelect:
                send(msg::select(g_tx, sizeof(g_tx), g_link, g_highlight < 0 ? 0 : g_highlight),
                     "select");
                break;
            case Action::SendContinue:
                send(msg::continue_(g_tx, sizeof(g_tx), g_link), "continue");
                break;
            case Action::PersistExecutionKey:
                persist_execution_key();
                break;
            case Action::ClearExecutionKey:
                clear_execution_key();
                break;
            case Action::SendStatusQuery:
                send(msg::status_query(g_tx, sizeof(g_tx), g_link), "status_query");
                break;
            case Action::SendActivity:
                send(msg::activity(g_tx, sizeof(g_tx), g_link), "activity");
                break;
            case Action::StartCapture:
                ++g_link.seg;
                send(msg::record_start(g_tx, sizeof(g_tx), g_link,
                                       t.to == State::RecordAddition),
                     "record_start");
                break;
            case Action::SendFinalMarker:
                // record_end is sent by the streamer, when the last chunk is
                // actually queued rather than when the button came up: on a
                // stalled link those are not the same moment, and the phone is
                // timing the delivery, not the gesture.
                break;
            default:
                break;
        }
    }
    if (!t.changed()) return;

    // A new request starts when a hold leaves READY. The counter is device local;
    // the phone maps it to the backend's request UUID.
    if (t.from == State::Ready && t.to == State::PhoneCheck) ++g_link.req;

    // Two of the four cancel reasons have no action in the V1 table, but the
    // transition says plainly that a request was abandoned, so the phone is told
    // rather than left watching a device that went quiet.
    if (!cancel_sent && is_pre_execution(t.from)) {
        if (t.to == State::CriticalBattery)
            send(msg::cancel(g_tx, sizeof(g_tx), g_link, msg::CancelReason::Battery), "cancel");
        else if (e.kind == EventKind::TransportFailure)
            send(msg::cancel(g_tx, sizeof(g_tx), g_link, msg::CancelReason::Transport), "cancel");
    }

    // Dismissed an uncertain execution: the key stays saved on both sides, so this
    // is not a cancel and must never be reported as one.
    if (t.from == State::ExecutionUncertain && e.kind == EventKind::DoublePress)
        send(msg::dismiss(g_tx, sizeof(g_tx), g_link), "dismiss");

    if (t.from == State::Wallpaper) send(msg::wake(g_tx, sizeof(g_tx)), "wake");

    // `ack` is any input acknowledging one of the three notice screens, plus a
    // press or the timeout on READ-ONLY RESULT.
    const bool notice = t.from == State::PhoneDisconnected || t.from == State::Success ||
                        t.from == State::Failure;
    if (t.to == State::Ready && ((notice && is_input_event(e.kind)) ||
                                 t.from == State::ReadOnlyResult))
        send(msg::ack(g_tx, sizeof(g_tx), g_link), "ack");
}

// Battery goes to the phone on threshold crossings, not on every reading: without
// it, a device entering CRITICAL BATTERY looks to the phone exactly like a device
// that went quiet.
void note_battery(uint8_t percent) {
    g_battery_pct = percent;
    g_link.battery = percent;
    const uint8_t bucket = percent > 50 ? 4 : percent > 25 ? 3 : percent > 10 ? 2 : 1;
    const uint8_t last = g_battery_sent > 100 ? 0 : g_battery_sent > 50   ? 4
                                               : g_battery_sent > 25 ? 3
                                               : g_battery_sent > 10 ? 2
                                                                     : 1;
    if (bucket != last) {
        g_battery_sent = percent;
        send(msg::battery(g_tx, sizeof(g_tx), percent), "battery");
    }
    feed(Event::at(EventKind::BatteryLevel, now_ms(), percent));
}

void send_hello() {
    // hello goes out here, not on connect: a notification sent before the phone
    // subscribes is dropped and nobody ever sees it.
    g_link.battery = g_battery_pct;
    send(msg::hello(g_tx, sizeof(g_tx), g_link, kFirmwareVersion, g_boot, g_machine.state(),
                    g_wallpaper.ready()),
         "hello");
    ESP_LOGI(kTag, "hello sent, MTU %u", ble::mtu());
}

void apply_settings(const char* js, size_t len) {
    const msg::SettingsUpdate u = msg::parse_settings(js, len, &g_settings);
    if (u.orientation || u.time || u.tz)
        ESP_LOGI(kTag, "settings: orientation=%s tz=%s time=%s",
                 g_settings.portrait ? "portrait" : "landscape", g_settings.tz, g_settings.time);
    if (!u.phone_ready) return;
    // PHONE / BLUETOOTH CHECK needs the link up AND the phone's capture service
    // ready. Only the pair means RECORDING.
    ESP_LOGI(kTag, "settings: phone_ready=%d", g_settings.phone_ready ? 1 : 0);
    feed(Event::at(g_settings.phone_ready && ble::connected() ? EventKind::PhoneReady
                                                              : EventKind::PhoneAbsent,
                   now_ms()));
}

// A wallpaper chunk. The receiver decides everything; this moves the bytes and
// says out loud what happened, because a transfer that quietly stops looks the
// same from the phone as one that is still going.
void apply_wallpaper(const char* js, size_t len) {
    const wp::Answer a = g_wallpaper_rx.feed(js, len);
    if (a.note[0] != '\0') {
        if (a.send && !a.ok) ESP_LOGW(kTag, "%s", a.note);
        else ESP_LOGI(kTag, "%s", a.note);
    }
    if (!a.send) return;

    // How many writes the radio actually handed us, against how far the receiver
    // got. Equal means nothing was lost on the way in and the gap is ours; fewer
    // means the write never arrived at all.
    ESP_LOGI(kTag, "wallpaper: %u writes arrived since the last answer, %u bytes accepted",
             static_cast<unsigned>(g_wallpaper_writes),
             static_cast<unsigned>(g_wallpaper_rx.received()));
    g_wallpaper_writes = 0;

    const uint32_t from = a.resend_from;
    send(msg::wallpaper_answer(g_tx, sizeof(g_tx), a.wp, a.ok, a.resend ? &from : nullptr),
         "wallpaper");
    // A stored wallpaper only reaches the glass on the next WALLPAPER screen, and
    // the user is looking at their phone right now, so nothing is redrawn here.
}

// A Screen write is the only way the device enters a decision or an outcome
// state: the backend owns those, and the device never invents one.
void apply_screen(const char* js, size_t len) {
    if (!msg::parse_screen(js, len, &g_screen)) {
        ESP_LOGW(kTag, "screen write rejected, previous screen kept");
        return;
    }
    if (g_screen.truncated)
        ESP_LOGE(kTag, "screen %ld carried more lines than the text budget allows: the "
                       "backend and the device disagree about the budget",
                 static_cast<long>(g_screen.sid));

    g_link.sid = g_screen.sid;
    if (g_screen.has_rev) g_link.rev = static_cast<uint8_t>(g_screen.rev & 0xFF);
    if (g_screen.has_pending_key)
        memcpy(g_link.pending_key, g_screen.pending_key, sizeof(g_link.pending_key));
    g_scroll = 0;
    g_highlight = g_screen.candidate_count > 0 ? 0 : -1;

    // A status_line on its own is the partial refresh path: the body is left
    // exactly as it is and only the status bar is redrawn.
    if (!g_screen.has_body) {
        if (g_screen.has_status) draw_screen(g_machine.state(), true);
        return;
    }

    EventKind kind = EventKind::Tick;
    if (msg::event_for_screen(g_screen.state, g_machine.state(), &kind)) {
        const Transition t = feed(Event::at(kind, now_ms()));
        if (!t.handled)
            ESP_LOGW(kTag, "screen %s ignored in %s: the device is not in a state that "
                           "screen belongs to",
                     msg::wire_name(g_screen.state), name(t.from));
        return;
    }
    // A screen with no event behind it (READY, CANCELLED, EXECUTING) still
    // replaces what is drawn.
    draw_screen(g_machine.state(), false);
}

// ---- the radio side: copy and hand over, never touch the machine ----
//
// These four run on the NimBLE host task. All they are allowed to do is copy.

Message g_staging;  // only ever touched on the NimBLE host task, so one is enough

void queue_message(Incoming kind, const char* data, size_t len) {
    if (g_inbox == nullptr) return;
    if (len > sizeof(g_staging.data)) {
        ESP_LOGE(kTag, "message of %u bytes is larger than the inbox slot: dropped",
                 static_cast<unsigned>(len));
        return;
    }
    g_staging.kind = kind;
    g_staging.len = static_cast<uint16_t>(len);
    if (len > 0) memcpy(g_staging.data, data, len);
    if (xQueueSend(g_inbox, &g_staging, 0) != pdTRUE)
        ESP_LOGE(kTag, "inbox full: a message from the phone was dropped");
}

// Screen and Settings arrive fragmented, each write carrying the 2 byte prefix.
// One reassembler per characteristic, so the two cannot interleave their message
// IDs; both live on the NimBLE host task and are touched from nowhere else. Only
// whole messages are handed to the main task.
msg::Reassembler g_screen_rx;
msg::Reassembler g_settings_rx;
msg::Reassembler g_wallpaper_frag;

void feed_fragment(msg::Reassembler& rx, Incoming kind, const char* what, const char* data,
                   size_t len) {
    const msg::Reassembler::Result r = rx.feed(data, len);
    if (rx.note()[0] != '\0') ESP_LOGW(kTag, "%s write: %s", what, rx.note());
    switch (r) {
        case msg::Reassembler::Result::Complete:
            queue_message(kind, rx.message(), rx.length());
            break;
        case msg::Reassembler::Result::Dropped:
            ESP_LOGE(kTag, "%s write dropped, nothing applied", what);
            break;
        case msg::Reassembler::Result::Incomplete:
            break;
    }
}

void on_screen(const char* js, size_t len) {
    feed_fragment(g_screen_rx, Incoming::Screen, "screen", js, len);
}
void on_settings(const char* js, size_t len) {
    feed_fragment(g_settings_rx, Incoming::Settings, "settings", js, len);
}
void on_wallpaper(const uint8_t* data, size_t len) {
    ++g_wallpaper_writes;
    feed_fragment(g_wallpaper_frag, Incoming::Wallpaper, "wallpaper",
                  reinterpret_cast<const char*>(data), len);
}
void on_events_subscribed() { queue_message(Incoming::EventsSubscribed, nullptr, 0); }
void on_phone_gone() {
    // A half assembled write from the old link must not be completed by the new
    // one: the IDs start over on a reconnect.
    g_screen_rx.reset();
    g_settings_rx.reset();
    g_wallpaper_frag.reset();
    queue_message(Incoming::Disconnected, nullptr, 0);
}

// Drained by the main loop, so everything below here runs on the task that owns
// the state machine.
void drain_inbox() {
    static Message m;
    while (g_inbox != nullptr && xQueueReceive(g_inbox, &m, 0) == pdTRUE) {
        switch (m.kind) {
            case Incoming::Screen: apply_screen(m.data, m.len); break;
            case Incoming::Settings: apply_settings(m.data, m.len); break;
            case Incoming::Wallpaper: apply_wallpaper(m.data, m.len); break;
            case Incoming::EventsSubscribed: send_hello(); break;
            case Incoming::Disconnected:
                // The link is the phone. Losing it while a request is in flight is
                // a transport failure, and the state machine decides what it costs.
                g_settings.phone_ready = false;
                // Transfer IDs start over on a reconnect, so a half written image
                // must never be finished by the next connection's chunks.
                g_wallpaper_rx.reset();
                feed(Event::at(EventKind::TransportFailure, now_ms()));
                break;
        }
    }
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
        case 'b': note_battery(4); break;   // critical
        case 'B': note_battery(80); break;  // recovered
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
                // The golden vectors in phone/test/golden/adpcm_chunks.json are
                // built with req 513, rev 2, seg 3, so the phone can compare the
                // bytes it receives against what it expects.
                // A new segment per press. The phone treats a repeat of a
                // finished segment as duplicates, correctly, so reusing seg 3
                // meant a board reset between runs (Carlos, 2026-10-20). The
                // first press after boot still uses seg 3, which is what the
                // golden vectors were generated with.
                static uint8_t clip_seg = 3;
                msg::Link clip = g_link;
                clip.req = 513;
                clip.rev = 2;
                clip.seg = clip_seg;
                send(msg::record_start(g_tx, sizeof(g_tx), clip, false), "record_start");
                g_audio.start(513, 2, clip_seg, pcm, samples);
                if (clip_seg < 255) ++clip_seg;
            } else {
                ESP_LOGW(kTag, "no phone connected");
            }
            break;
        // Framebuffer patterns, bypassing the rasteriser. 1 and 2 are taken by
        // the gesture injection, so these are letters.
        case 'q': board::debug_pattern(0); break;  // all black
        case 'w': board::debug_pattern(1); break;  // all white
        case 'e': board::debug_pattern(2); break;  // left half black
        case 'd': board::debug_pattern(3); break;  // right half black
        case 'z': board::debug_pattern(4); break;  // top half black
        case 'v': board::debug_pattern(5); break;  // square at the origin
        case 'C':
            ESP_LOGI(kTag, "deep cleaning the panel, a few seconds ...");
            board::deep_clean(4);
            ESP_LOGI(kTag, "panel cleaned");
            break;
        case 'T': {
            // Full panel probe. The old one drew five short lines starting at
            // x=48, so every line ended around x=350 of 792 and the right hand
            // controller was legitimately given nothing to draw: it could not
            // tell a half dead panel from a short string. This one covers every
            // row and both controllers, and labels the extremes, so a region
            // that does not update can be named rather than guessed at.
            static DisplayList probe;
            probe.clear();
            const Caps caps = board::display().caps();
            const TextMetrics tm = board::display().metrics(TextStyle::Body);
            const int cols = tm.char_w ? caps.logical_w / tm.char_w - 1 : 0;
            const int rows = tm.line_h ? caps.visible_h / tm.line_h : 0;

            static char lines[12][80];
            const int n = rows > 12 ? 12 : rows;
            const int w = cols > 78 ? 78 : cols;
            for (int r = 0; r < n; ++r) {
                for (int i = 0; i < w; ++i)
                    lines[r][i] = (i % 10 == 0) ? static_cast<char>('0' + (i / 10) % 10) : '-';
                // Both ends carry the row letter, so a row that stops half way
                // across is obvious: you see A but not the A that closes it.
                lines[r][0] = static_cast<char>('A' + r);
                if (w > 1) lines[r][w - 1] = static_cast<char>('A' + r);
                lines[r][w] = '\0';
                TextCmd t;
                t.box = Box{0, static_cast<int16_t>(r * tm.line_h), caps.logical_w, tm.line_h};
                t.text = lines[r];
                probe.text(t);
            }

            // Solid blocks just inside each corner of the visible area.
            const int16_t right = static_cast<int16_t>(caps.logical_w - 40);
            const int16_t bottom = static_cast<int16_t>(caps.visible_h - 24);
            probe.rect(RectCmd{Box{0, 0, 24, 24}, true, 0});
            probe.rect(RectCmd{Box{right, 0, 24, 24}, true, 0});
            probe.rect(RectCmd{Box{0, bottom, 24, 24}, true, 0});
            probe.rect(RectCmd{Box{right, bottom, 24, 24}, true, 0});

            board::display().draw(probe);
            if (probe.dropped())
                ESP_LOGE(kTag, "probe dropped %u commands: the display list is full",
                         probe.dropped());
            board::display().present(Refresh::Full, nullptr, nullptr);
            ESP_LOGI(kTag, "full panel probe: rows A to %c, %d columns, a black block inside "
                           "each of the four corners. Every row should start and end with its "
                           "own letter.",
                     static_cast<char>('A' + n - 1), w);
            break;
        }
        case '?':
            ESP_LOGI(kTag, "p/P phone ready|absent, a audio acked, r/c/s/o results, "
                           "k/f/u execution, x transport fail, b/B battery, "
                           "A stream the clip, q/w/e/d/z/v patterns, C clean, T text probe");
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
    ESP_LOGI(kTag, "HaloAI device, %s. Panel: %s", kFirmwareVersion, board::panel_name());
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

    // Random per power up, so the phone can tell a reconnect (same boot) from a
    // restart (new boot), which matters because req and seg start over.
    g_boot = esp_random();

    // NVS is where the pending execution key will live in F3; NimBLE needs it too.
    if (nvs_flash_init() == ESP_ERR_NVS_NO_FREE_PAGES) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    // Created before the radio starts, so no callback can arrive without somewhere
    // to put its message.
    // 12 deep, not 4: a wallpaper is 341 writes back to back, and the main loop
    // stops draining for tens of ms whenever it erases a flash sector. Every
    // message dropped there costs the phone a resend of everything after it.
    g_inbox = xQueueCreate(12, sizeof(Message));
    if (g_inbox == nullptr) ESP_LOGE(kTag, "no inbox: messages from the phone will be dropped");

    // Before the radio starts: `hello` reports whether a wallpaper can be stored
    // at all, and the phone keeps its Send button off until it says yes.
    g_wallpaper.init();

    ble::Handlers h;
    h.on_audio_ack = on_audio_ack;
    h.on_screen = on_screen;
    h.on_settings = on_settings;
    h.on_wallpaper = on_wallpaper;
    h.on_disconnect = on_phone_gone;
    h.on_events_subscribed = on_events_subscribed;
    ble::start("HaloAI", h);

    // BOOT / RECOVERY. A key in flash means an action was dispatched and its
    // outcome was never confirmed, so the device reconciles it before anything
    // else rather than returning to READY as if nothing had happened.
    if (load_execution_key()) {
        ESP_LOGW(kTag, "pending execution found in flash: %s", g_link.pending_key);
        feed(Event::at(EventKind::PendingExecutionFound, now_ms()));
    } else {
        feed(Event::at(EventKind::StartupOk, now_ms()));
    }

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

        drain_inbox();
        g_audio.poll();
        // Through feed(), not g_machine.tick(): the transition carries the
        // actions, and feed() is the only thing that applies them. Calling tick()
        // and dropping what it returned moved the state and drew nothing, told
        // the phone nothing and logged nothing, so READY reached WALLPAPER
        // without the picture appearing, a decision state expired without the
        // cancel being sent, and FINALIZE AUDIO passed its delivery deadline into
        // FAILURE with the screen still saying FINALIZE AUDIO. Found on the board
        // 2026-10-25: `hello` answered "state":"WALLPAPER" while the glass still
        // showed READY. An idle tick is unhandled and is deliberately not logged,
        // so this costs nothing 200 times a second.
        feed(Event::at(EventKind::Tick, t));
        handle_serial();
        // Never let this round to zero ticks: that starves the idle task and
        // trips the watchdog. See CONFIG_FREERTOS_HZ in sdkconfig.defaults.
        constexpr TickType_t kDelay = pdMS_TO_TICKS(kPollMs) > 0 ? pdMS_TO_TICKS(kPollMs) : 1;
        vTaskDelay(kDelay);
    }
}
