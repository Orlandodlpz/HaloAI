#include "halo/state_machine.hpp"

namespace halo {

namespace {

bool is_input(EventKind k) {
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

bool is_rotate(EventKind k) {
    return k == EventKind::RotateUp || k == EventKind::RotateDown;
}

bool is_capturing(State s) {
    return s == State::Recording || s == State::RecordAddition;
}

// Work on a request that has NOT been dispatched yet. The backend can still
// cancel these. Once approve has gone out (EXECUTING onwards) it cannot: the
// effect may already exist, and those are reconciled by execution key instead.
bool is_before_dispatch(State s) {
    switch (s) {
        case State::PhoneCheck:
        case State::Recording:
        case State::FinalizeAudio:
        case State::Processing:
        case State::ProcessingSlow:
        case State::RecordAddition:
        case State::ConflictAmbiguous:
        case State::ConflictSchedule:
        case State::Review:
            return true;
        default:
            return false;
    }
}

}  // namespace

Transition StateMachine::stay(const Event& e, const char* note, bool handled) {
    Transition t;
    t.from = state_;
    t.to = state_;
    t.handled = handled;
    t.note = note;
    (void)e;
    return t;
}

Transition StateMachine::go(State to, const Event& e, const char* note,
                            std::initializer_list<Action> pre) {
    Transition t;
    t.from = state_;
    t.to = to;
    t.handled = true;
    t.note = note;
    for (Action a : pre) t.actions.add(a);
    enter(to, e.at_ms, t.actions);
    state_ = to;
    return t;
}

void StateMachine::enter(State to, uint32_t at, ActionList& out) {
    const State from = state_;
    entered_at_ = at;
    deadline_at_ = 0;
    expires_at_ = 0;
    if (to != State::Review) {
        review_presented_ = false;
        guard_until_ = 0;
    }

    switch (to) {
        case State::Ready:
            last_activity_ = at;
            break;
        case State::FinalizeAudio:
            deadline_at_ = at + cfg_.delivery_deadline_ms;
            break;
        case State::Processing:
            // The processing deadline belongs to the revision, not to the state, so
            // bouncing through PROCESSING SLOW and back does not extend it.
            if (from != State::ProcessingSlow)
                processing_deadline_at_ = at + cfg_.processing_deadline_ms;
            deadline_at_ = processing_deadline_at_;
            slow_at_ = at + cfg_.processing_slow_ms;
            break;
        case State::ProcessingSlow:
            deadline_at_ = processing_deadline_at_;
            break;
        case State::Executing:
        case State::ReconcileStatus:
            deadline_at_ = at + cfg_.execution_timeout_ms;
            break;
        case State::Success:
        case State::Failure:
            deadline_at_ = at + cfg_.outcome_timeout_ms;
            break;
        case State::ReadOnlyResult:
            deadline_at_ = at + cfg_.read_only_timeout_ms;
            break;
        case State::Cancelled:
            deadline_at_ = at + cfg_.cancelled_notice_ms;
            break;
        case State::ConflictAmbiguous:
        case State::ConflictSchedule:
        case State::Review:
            expires_at_ = at + cfg_.decision_expiry_ms;
            break;
        default:
            break;
    }

    // Entry actions that are a property of the state itself.
    if (is_capturing(to)) out.add(Action::StartCapture);
    if (to == State::FinalizeAudio) out.add(Action::SendFinalMarker);
    if (to == State::CriticalBattery) out.add(Action::InhibitCapture);
    out.add(to == State::Wallpaper ? Action::ShowWallpaper : Action::DrawScreen);
}

bool StateMachine::rate_limited_activity(uint32_t at) {
    if (activity_sent_once_ && at - activity_sent_at_ < cfg_.activity_rate_limit_ms) return false;
    activity_sent_once_ = true;
    activity_sent_at_ = at;
    return true;
}

Transition StateMachine::handle(const Event& e) {
    // ---- Global rule 1: battery first, from every state ----
    if (e.kind == EventKind::BatteryLevel) {
        const bool critical = e.value <= static_cast<int32_t>(cfg_.critical_battery_pct);
        if (critical && state_ != State::CriticalBattery) {
            // A dispatched action's key stays saved: never cleared here.
            if (is_capturing(state_))
                return go(State::CriticalBattery, e, "battery critical", {Action::StopCapture});
            return go(State::CriticalBattery, e, "battery critical");
        }
        if (!critical && state_ == State::CriticalBattery) {
            return pending_execution_
                       ? go(State::ReconcileStatus, e, "battery recovered, pending execution",
                            {Action::SendStatusQuery})
                       : go(State::Ready, e, "battery recovered");
        }
        return stay(e, "battery level", true);
    }
    if (state_ == State::CriticalBattery) {
        // Capture and execution are inhibited; nothing else moves this state.
        return stay(e, "inhibited by critical battery", false);
    }

    // ---- Timeouts ----
    if (e.kind == EventKind::Tick) {
        if (state_ == State::Ready && e.at_ms - last_activity_ > cfg_.wallpaper_idle_ms)
            return go(State::Wallpaper, e, "idle > 1 min");
        if (is_decision_state(state_) && e.at_ms >= expires_at_)
            return go(State::Cancelled, e, "60 s expiry", {Action::SendCancel});
        if (state_ == State::Processing && e.at_ms >= slow_at_)
            return go(State::ProcessingSlow, e, "slow threshold");
        if (deadline_at_ != 0 && e.at_ms >= deadline_at_) {
            switch (state_) {
                case State::FinalizeAudio:
                    return go(State::Failure, e, "delivery deadline");
                case State::Processing:
                case State::ProcessingSlow:
                    return go(State::Failure, e, "processing deadline");
                case State::Executing:
                    return go(State::ExecutionUncertain, e, "execution timeout");
                case State::ReconcileStatus:
                    return go(State::ExecutionUncertain, e, "status inconclusive");
                case State::Success:
                case State::Failure:
                    return go(State::Ready, e, "outcome timeout");
                case State::ReadOnlyResult:
                    return go(State::Ready, e, "result timeout");
                case State::Cancelled:
                    return go(State::Ready, e, "cancellation notice complete");
                default:
                    break;
            }
        }
        return stay(e, "", false);
    }

    // ---- The backend cancelled this request (team addition 2026-10-17) ----
    //
    // Two different things wear the same screen. In RECONCILE STATUS the phone
    // only sends it after the status endpoint confirmed the key was never
    // approved and the cancel went through, so the device knows nothing happened
    // and may drop the key. Everywhere else it is the ordinary case: the request
    // expired on the backend, or a newer one replaced it.
    if (e.kind == EventKind::ScreenCancelled) {
        if (state_ == State::ReconcileStatus) {
            pending_execution_ = false;
            return go(State::Cancelled, e, "never approved, request cancelled",
                      {Action::ClearExecutionKey});
        }
        if (is_before_dispatch(state_)) {
            if (is_capturing(state_))
                return go(State::Cancelled, e, "cancelled upstream", {Action::StopCapture});
            return go(State::Cancelled, e, "cancelled upstream");
        }
        // Nothing of ours is in flight, or it is already dispatched and cannot be
        // cancelled. Never acted on, so a stray one cannot throw away a key.
        return stay(e, "nothing to cancel here", false);
    }

    // ---- Per state ----
    switch (state_) {
        case State::BootRecovery:
            if (e.kind == EventKind::StartupOk) return go(State::Ready, e, "startup checks passed");
            if (e.kind == EventKind::PendingExecutionFound) {
                pending_execution_ = true;
                return go(State::ReconcileStatus, e, "recovered pending execution",
                          {Action::SendStatusQuery});
            }
            return stay(e, "", false);

        case State::Ready:
            if (e.kind == EventKind::HoldStart) return go(State::PhoneCheck, e, "hold: new request");
            if (is_input(e.kind)) {
                last_activity_ = e.at_ms;
                return stay(e, "activity");
            }
            return stay(e, "", false);

        case State::Wallpaper:
            // Press or rotate wakes, and that input is consumed: a hold here wakes
            // the screen, it does not start a recording.
            if (is_input(e.kind)) return go(State::Ready, e, "wake, input consumed");
            return stay(e, "", false);

        case State::PhoneCheck:
            if (e.kind == EventKind::PhoneReady) return go(State::Recording, e, "phone ready");
            if (e.kind == EventKind::PhoneAbsent)
                return go(State::PhoneDisconnected, e, "phone absent");
            if (e.kind == EventKind::DoublePress) return go(State::Cancelled, e, "cancel");
            // Released before the phone answered: nothing was captured, nothing sent.
            if (e.kind == EventKind::Release) return go(State::Cancelled, e, "released early");
            return stay(e, "", false);

        case State::PhoneDisconnected:
            if (e.kind == EventKind::DoublePress) return go(State::Cancelled, e, "cancel");
            if (is_input(e.kind)) return go(State::Ready, e, "acknowledged");
            return stay(e, "", false);

        case State::Recording:
            if (e.kind == EventKind::Release)
                return go(State::FinalizeAudio, e, "release: close segment", {Action::StopCapture});
            if (e.kind == EventKind::TransportFailure)
                return go(State::Failure, e, "link loss or buffer exhaustion", {Action::StopCapture});
            if (e.kind == EventKind::DoublePress)
                return go(State::Cancelled, e, "cancel", {Action::StopCapture, Action::SendCancel});
            return stay(e, "", false);

        case State::FinalizeAudio:
            if (e.kind == EventKind::AudioComplete)
                return go(State::Processing, e, "all chunks and final marker acknowledged");
            if (e.kind == EventKind::TransportFailure)
                return go(State::Failure, e, "transport failure");
            if (e.kind == EventKind::DoublePress)
                return go(State::Cancelled, e, "cancel", {Action::SendCancel});
            return stay(e, "", false);

        case State::Processing:
        case State::ProcessingSlow: {
            if (e.kind == EventKind::ProcessingSlow && state_ == State::Processing)
                return go(State::ProcessingSlow, e, "backend slow event");
            if (e.kind == EventKind::ProcessingProgress && state_ == State::ProcessingSlow)
                return go(State::Processing, e, "progress resumed");
            if (e.kind == EventKind::ProcessingFailed)
                return go(State::Failure, e, "model or transport error");
            if (e.kind == EventKind::HoldStart)
                return go(State::RecordAddition, e, "hold: add to this request");
            if (e.kind == EventKind::DoublePress)
                return go(State::Cancelled, e, "cancel", {Action::SendCancel});
            // Results route the same way from both states. The V1 table lists the
            // result rows only under PROCESSING, but a result that arrives while the
            // screen says PROCESSING SLOW has to be shown, not stranded.
            if (e.kind == EventKind::ScreenConflictAmbiguous)
                return go(State::ConflictAmbiguous, e, "result: ambiguous");
            if (e.kind == EventKind::ScreenConflictSchedule)
                return go(State::ConflictSchedule, e, "result: schedule conflict");
            if (e.kind == EventKind::ScreenReview) return go(State::Review, e, "result: proposal");
            if (e.kind == EventKind::ScreenReadOnly)
                return go(State::ReadOnlyResult, e, "result: read only");
            return stay(e, "", false);
        }

        case State::RecordAddition:
            if (e.kind == EventKind::Release)
                return go(State::FinalizeAudio, e, "release: close addition", {Action::StopCapture});
            if (e.kind == EventKind::TransportFailure)
                return go(State::Failure, e, "link loss or buffer exhaustion", {Action::StopCapture});
            if (e.kind == EventKind::DoublePress)
                return go(State::Cancelled, e, "cancel", {Action::StopCapture, Action::SendCancel});
            return stay(e, "", false);

        case State::ConflictAmbiguous:
            if (e.kind == EventKind::HoldStart)
                return go(State::RecordAddition, e, "hold: answer by voice");
            if (is_rotate(e.kind)) {
                expires_at_ = e.at_ms + cfg_.decision_expiry_ms;
                Transition t = stay(e, "move highlight");
                if (rate_limited_activity(e.at_ms)) t.actions.add(Action::SendActivity);
                return t;
            }
            if (e.kind == EventKind::SinglePress)
                return go(State::Processing, e, "select candidate", {Action::SendSelect});
            if (e.kind == EventKind::DoublePress)
                return go(State::Cancelled, e, "cancel", {Action::SendCancel});
            return stay(e, "", false);

        case State::ConflictSchedule:
            if (e.kind == EventKind::HoldStart)
                return go(State::RecordAddition, e, "hold: change details by voice");
            if (is_rotate(e.kind)) {
                expires_at_ = e.at_ms + cfg_.decision_expiry_ms;
                // A scroll moves the window over the body, so the glass is
                // wrong until it is redrawn. Without this the scroll position
                // advanced and nothing ever appeared to happen: the knob was
                // dead on every screen longer than the panel.
                Transition t = stay(e, "scroll");
                t.actions.add(Action::DrawScreen);
                if (rate_limited_activity(e.at_ms)) t.actions.add(Action::SendActivity);
                return t;
            }
            if (e.kind == EventKind::SinglePress)
                return go(State::Review, e, "continue despite conflict", {Action::SendContinue});
            if (e.kind == EventKind::DoublePress)
                return go(State::Cancelled, e, "cancel", {Action::SendCancel});
            return stay(e, "", false);

        case State::Review:
            if (e.kind == EventKind::ScreenPresented) {
                // The guard starts when the pixels are on the panel, not when the
                // screen was queued. On e-paper those are hundreds of ms apart.
                review_presented_ = true;
                guard_until_ = e.at_ms + cfg_.confirm_guard_ms;
                return stay(e, "review drawn, confirm guard started");
            }
            if (e.kind == EventKind::HoldStart)
                return go(State::RecordAddition, e, "hold: revise by voice");
            if (is_rotate(e.kind)) {
                expires_at_ = e.at_ms + cfg_.decision_expiry_ms;
                Transition t = stay(e, "scroll proposal");
                t.actions.add(Action::DrawScreen);
                if (rate_limited_activity(e.at_ms)) t.actions.add(Action::SendActivity);
                return t;
            }
            if (e.kind == EventKind::SinglePress) {
                // The only path in the whole machine that dispatches a mutating
                // action. It is closed until the REVIEW screen has been presented
                // and the guard has elapsed.
                if (!confirm_guard_open(e.at_ms)) return stay(e, "ignored by confirm guard", true);
                pending_execution_ = true;
                return go(State::Executing, e, "approve displayed revision",
                          {Action::PersistExecutionKey, Action::SendApprove});
            }
            if (e.kind == EventKind::DoublePress)
                return go(State::Cancelled, e, "cancel", {Action::SendCancel});
            return stay(e, "", false);

        case State::ReadOnlyResult:
            if (is_rotate(e.kind)) {
                Transition t = stay(e, "scroll result");
                t.actions.add(Action::DrawScreen);
                return t;
            }
            if (e.kind == EventKind::SinglePress) return go(State::Ready, e, "acknowledged");
            return stay(e, "", false);

        case State::Executing:
            if (e.kind == EventKind::ExecutionApplied) {
                pending_execution_ = false;
                return go(State::Success, e, "authoritative success", {Action::ClearExecutionKey});
            }
            if (e.kind == EventKind::ExecutionRejected) {
                pending_execution_ = false;
                return go(State::Failure, e, "authoritative rejection", {Action::ClearExecutionKey});
            }
            if (e.kind == EventKind::ExecutionUnknown)
                return go(State::ExecutionUncertain, e, "ambiguous response");
            if (e.kind == EventKind::TransportFailure)
                return go(State::ExecutionUncertain, e, "link loss while executing");
            // Stops waiting only. The effect may already exist, so the key stays.
            if (e.kind == EventKind::DoublePress)
                return go(State::ExecutionUncertain, e, "stopped waiting");
            return stay(e, "", false);

        case State::ExecutionUncertain:
            // An authoritative answer can arrive here without being asked for:
            // the phone polls the status endpoint while the device waits. Section
            // 2 (2026-10-19) says the key is cleared on an outcome in this state
            // too, and ignoring one would leave a key in flash for an action the
            // backend has already settled.
            if (e.kind == EventKind::ExecutionApplied) {
                pending_execution_ = false;
                return go(State::Success, e, "authoritative success while uncertain",
                          {Action::ClearExecutionKey});
            }
            if (e.kind == EventKind::ExecutionRejected) {
                pending_execution_ = false;
                return go(State::Failure, e, "authoritative rejection while uncertain",
                          {Action::ClearExecutionKey});
            }
            if (e.kind == EventKind::SinglePress || e.kind == EventKind::StatusQueryDue)
                return go(State::ReconcileStatus, e, "query the same execution key",
                          {Action::SendStatusQuery});
            if (e.kind == EventKind::DoublePress)
                return go(State::Ready, e, "dismissed, action stays saved");
            return stay(e, "", false);

        case State::ReconcileStatus:
            if (e.kind == EventKind::ExecutionApplied) {
                pending_execution_ = false;
                return go(State::Success, e, "confirmed applied", {Action::ClearExecutionKey});
            }
            if (e.kind == EventKind::ExecutionRejected) {
                pending_execution_ = false;
                return go(State::Failure, e, "confirmed not applied", {Action::ClearExecutionKey});
            }
            if (e.kind == EventKind::ExecutionUnknown)
                return go(State::ExecutionUncertain, e, "still inconclusive");
            return stay(e, "", false);

        case State::Success:
        case State::Failure:
            if (is_input(e.kind)) return go(State::Ready, e, "acknowledged");
            return stay(e, "", false);

        case State::Cancelled:
            if (e.kind == EventKind::CancelAck)
                return go(State::Ready, e, "cancellation notice complete");
            // Any input dismisses the notice, like SUCCESS and FAILURE (team
            // addition 2026-10-17). Waiting out the timer is not the only way.
            if (is_input(e.kind)) return go(State::Ready, e, "acknowledged");
            return stay(e, "", false);

        default:
            return stay(e, "", false);
    }
}

}  // namespace halo
