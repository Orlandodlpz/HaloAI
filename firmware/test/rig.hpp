// Shared fixture: a state machine plus a fake clock, and a way to drive it into
// any of the 21 states so each test starts where it means to start.
#pragma once

#include "halo/state_machine.hpp"

namespace halo::test {

struct Rig {
    Config cfg{};
    StateMachine sm{cfg};
    uint32_t now = 1000;

    Rig() = default;
    Rig(const Rig&) = delete;             // sm holds a reference to cfg
    Rig& operator=(const Rig&) = delete;

    Transition send(EventKind k, int32_t value = 0) {
        now += 1;
        return sm.handle(Event::at(k, now, value));
    }
    // Jump forward and tick once. Deadlines are absolute, so one tick is enough.
    Transition advance(uint32_t ms) {
        now += ms;
        return sm.tick(now);
    }
};

inline void drive_to(Rig& r, State target) {
    using E = EventKind;
    if (target == State::BootRecovery) return;
    if (target == State::CriticalBattery) {
        r.send(E::StartupOk);
        r.send(E::BatteryLevel, 5);
        return;
    }
    r.send(E::StartupOk);  // READY
    if (target == State::Ready) return;
    if (target == State::Wallpaper) {
        r.advance(r.cfg.wallpaper_idle_ms + 1);
        return;
    }
    r.send(E::HoldStart);  // PHONE / BLUETOOTH CHECK
    if (target == State::PhoneCheck) return;
    if (target == State::PhoneDisconnected) {
        r.send(E::PhoneAbsent);
        return;
    }
    r.send(E::PhoneReady);  // RECORDING
    if (target == State::Recording) return;
    r.send(E::Release);  // FINALIZE AUDIO
    if (target == State::FinalizeAudio) return;
    r.send(E::AudioComplete);  // PROCESSING
    if (target == State::Processing) return;
    switch (target) {
        case State::ProcessingSlow: r.send(E::ProcessingSlow); return;
        case State::RecordAddition: r.send(E::HoldStart); return;
        case State::ConflictAmbiguous: r.send(E::ScreenConflictAmbiguous); return;
        case State::ConflictSchedule: r.send(E::ScreenConflictSchedule); return;
        case State::ReadOnlyResult: r.send(E::ScreenReadOnly); return;
        case State::Cancelled: r.send(E::DoublePress); return;
        default: break;
    }
    r.send(E::ScreenReview);  // REVIEW, confirm guard still closed
    if (target == State::Review) return;
    r.send(E::ScreenPresented);
    r.advance(r.cfg.confirm_guard_ms + 1);
    r.send(E::SinglePress);  // EXECUTING
    if (target == State::Executing) return;
    switch (target) {
        case State::Success: r.send(E::ExecutionApplied); return;
        case State::Failure: r.send(E::ExecutionRejected); return;
        default: break;
    }
    r.send(E::ExecutionUnknown);  // EXECUTION UNCERTAIN
    if (target == State::ExecutionUncertain) return;
    r.send(E::SinglePress);  // RECONCILE STATUS
}

}  // namespace halo::test
