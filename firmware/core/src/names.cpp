// Names for the serial log and the test output. Spelled like the spec's tables so
// a log line can be compared with BRIEFING.md section 2 without translating.
#include "halo/events.hpp"
#include "halo/states.hpp"

namespace halo {

const char* name(State s) {
    switch (s) {
        case State::BootRecovery: return "BOOT / RECOVERY";
        case State::Ready: return "READY";
        case State::Wallpaper: return "WALLPAPER";
        case State::CriticalBattery: return "CRITICAL BATTERY";
        case State::PhoneCheck: return "PHONE / BLUETOOTH CHECK";
        case State::PhoneDisconnected: return "PHONE DISCONNECTED";
        case State::Recording: return "RECORDING";
        case State::FinalizeAudio: return "FINALIZE AUDIO";
        case State::Processing: return "PROCESSING";
        case State::ProcessingSlow: return "PROCESSING SLOW";
        case State::RecordAddition: return "RECORD ADDITION";
        case State::ConflictAmbiguous: return "CONFLICT-AMBIGUOUS";
        case State::ConflictSchedule: return "CONFLICT-SCHEDULE";
        case State::Review: return "REVIEW";
        case State::ReadOnlyResult: return "READ-ONLY RESULT";
        case State::Executing: return "EXECUTING";
        case State::ExecutionUncertain: return "EXECUTION UNCERTAIN";
        case State::ReconcileStatus: return "RECONCILE STATUS";
        case State::Success: return "SUCCESS";
        case State::Failure: return "FAILURE";
        case State::Cancelled: return "CANCELLED";
        default: return "?";
    }
}

bool is_active_work(State s) {
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
        case State::Executing:
        case State::ExecutionUncertain:
        case State::ReconcileStatus:
            return true;
        default:
            return false;
    }
}

bool is_decision_state(State s) {
    return s == State::ConflictAmbiguous || s == State::ConflictSchedule || s == State::Review;
}

const char* name(EventKind k) {
    switch (k) {
        case EventKind::SinglePress: return "single press";
        case EventKind::DoublePress: return "double press";
        case EventKind::HoldStart: return "hold";
        case EventKind::Release: return "release";
        case EventKind::RotateUp: return "rotate up";
        case EventKind::RotateDown: return "rotate down";
        case EventKind::Wake: return "wake";
        case EventKind::Tick: return "tick";
        case EventKind::BatteryLevel: return "battery";
        case EventKind::StartupOk: return "startup ok";
        case EventKind::PendingExecutionFound: return "pending execution found";
        case EventKind::PhoneReady: return "phone ready";
        case EventKind::PhoneAbsent: return "phone absent";
        case EventKind::AudioComplete: return "audio acknowledged";
        case EventKind::TransportFailure: return "transport failure";
        case EventKind::ScreenPresented: return "screen presented";
        case EventKind::ScreenConflictAmbiguous: return "screen: ambiguous";
        case EventKind::ScreenConflictSchedule: return "screen: schedule conflict";
        case EventKind::ScreenReview: return "screen: review";
        case EventKind::ScreenReadOnly: return "screen: read only";
        case EventKind::ProcessingSlow: return "slow";
        case EventKind::ProcessingProgress: return "progress";
        case EventKind::ProcessingFailed: return "processing failed";
        case EventKind::ExecutionApplied: return "execution applied";
        case EventKind::ExecutionRejected: return "execution rejected";
        case EventKind::ExecutionUnknown: return "execution unknown";
        case EventKind::CancelAck: return "cancel acknowledged";
        case EventKind::StatusQueryDue: return "status query due";
        default: return "?";
    }
}

const char* name(Action a) {
    switch (a) {
        case Action::DrawScreen: return "draw screen";
        case Action::StartCapture: return "start capture";
        case Action::StopCapture: return "stop capture";
        case Action::SendFinalMarker: return "send final marker";
        case Action::InhibitCapture: return "inhibit capture";
        case Action::PersistExecutionKey: return "persist execution key";
        case Action::ClearExecutionKey: return "clear execution key";
        case Action::SendApprove: return "send approve";
        case Action::SendCancel: return "send cancel";
        case Action::SendSelect: return "send select";
        case Action::SendContinue: return "send continue";
        case Action::SendStatusQuery: return "send status query";
        case Action::SendActivity: return "send activity";
        case Action::ShowWallpaper: return "show wallpaper";
        default: return "?";
    }
}

}  // namespace halo
