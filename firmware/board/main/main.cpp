// F0 on real hardware: read the buttons and the knob, turn them into V1 gestures,
// run the state machine, and log every input and every transition over serial.
//
// No BLE and no screen rendering yet. Everything the phone would send is injected
// over serial instead (type a letter in idf.py monitor), so all 21 states can be
// walked on the bench before any radio exists.
#include <cstdio>

#include "board_pins.hpp"
#include "display_backends.hpp"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "halo/gestures.hpp"
#include "halo/layout.hpp"
#include "halo/state_machine.hpp"

using namespace halo;

namespace {

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
        ScreenModel m;
        m.state = t.to;
        m.status_line = name(t.to);
        DisplayList list;
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
};

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

        // Knob: one event per edge, not per poll.
        for (Edge* e : {&up, &down, &knob}) {
            const bool down_now = pressed(e->pin);
            if (down_now && !e->was_down) {
                if (e->pin == board::kKnobUp) feed(Event::at(EventKind::RotateUp, t, 1));
                else if (e->pin == board::kKnobDown) feed(Event::at(EventKind::RotateDown, t, -1));
                else feed(Event::at(EventKind::Wake, t));
            }
            e->was_down = down_now;
        }

        const bool exit_now = pressed(exitb.pin);
        if (exit_now && !exitb.was_down) ESP_LOGI(kTag, "%8lu  EXIT button (unmapped)",
                                                  static_cast<unsigned long>(t));
        exitb.was_down = exit_now;

        g_machine.tick(t);
        handle_serial();
        // Never let this round to zero ticks: that starves the idle task and
        // trips the watchdog. See CONFIG_FREERTOS_HZ in sdkconfig.defaults.
        constexpr TickType_t kDelay = pdMS_TO_TICKS(kPollMs) > 0 ? pdMS_TO_TICKS(kPollMs) : 1;
        vTaskDelay(kDelay);
    }
}
