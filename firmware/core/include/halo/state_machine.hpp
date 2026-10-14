// The V1 device state machine (BRIEFING.md section 2, plus the team additions).
//
// Hardware independent on purpose: it owns no clock, no pins, no radio and no
// display. Feed it events that carry their own timestamp, carry out the actions
// it returns. Every transition in both tables is covered by a host test.
//
// Two safety properties this class is responsible for, and which the tests pin:
//  1. Only EventKind::SinglePress in State::Review produces Action::SendApprove,
//     and only after the confirm guard opened. The guard opens on
//     EventKind::ScreenPresented, which the display backend sends when present()
//     returns, so on e-paper the guard starts when the pixels are actually there,
//     not when the screen was queued.
//  2. A battery level at or below the critical threshold moves every state to
//     CRITICAL BATTERY, stops capture and blocks new execution.
#pragma once

#include <initializer_list>

#include "halo/config.hpp"
#include "halo/events.hpp"
#include "halo/states.hpp"

namespace halo {

struct Transition {
    State from = State::BootRecovery;
    State to = State::BootRecovery;
    ActionList actions{};
    bool handled = false;       // false: the event means nothing in this state
    const char* note = "";      // short reason, for the serial log
    bool changed() const { return from != to; }
};

class StateMachine {
public:
    explicit StateMachine(const Config& cfg) : cfg_(cfg) {}

    State state() const { return state_; }
    bool has_pending_execution() const { return pending_execution_; }

    // The only way in. Returns what changed and what the hardware layer must do.
    Transition handle(const Event& e);

    Transition tick(uint32_t at_ms) { return handle(Event::at(EventKind::Tick, at_ms)); }

    // Test and log helpers.
    uint32_t deadline_at() const { return deadline_at_; }
    uint32_t expires_at() const { return expires_at_; }
    bool confirm_guard_open(uint32_t at_ms) const {
        return review_presented_ && at_ms >= guard_until_;
    }

private:
    Transition stay(const Event& e, const char* note, bool handled = true);
    Transition go(State to, const Event& e, const char* note,
                  std::initializer_list<Action> pre = {});
    void enter(State to, uint32_t at_ms, ActionList& out);
    bool rate_limited_activity(uint32_t at_ms);

    const Config& cfg_;
    State state_ = State::BootRecovery;

    uint32_t entered_at_ = 0;
    uint32_t deadline_at_ = 0;            // 0: this state has no deadline
    uint32_t expires_at_ = 0;             // decision states only
    uint32_t processing_deadline_at_ = 0; // survives PROCESSING <-> PROCESSING SLOW
    uint32_t slow_at_ = 0;
    uint32_t guard_until_ = 0;
    uint32_t last_activity_ = 0;          // READY inactivity, for WALLPAPER
    uint32_t activity_sent_at_ = 0;
    bool activity_sent_once_ = false;
    bool review_presented_ = false;
    bool pending_execution_ = false;
};

}  // namespace halo
