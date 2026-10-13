// The 21 states of the V1 device state machine (BRIEFING.md section 2).
// Names here map one to one onto the spec's state names.
#pragma once

#include <cstdint>

namespace halo {

enum class State : uint8_t {
    BootRecovery,        // BOOT / RECOVERY
    Ready,               // READY
    Wallpaper,           // WALLPAPER
    CriticalBattery,     // CRITICAL BATTERY
    PhoneCheck,          // PHONE / BLUETOOTH CHECK
    PhoneDisconnected,   // PHONE DISCONNECTED
    Recording,           // RECORDING
    FinalizeAudio,       // FINALIZE AUDIO
    Processing,          // PROCESSING
    ProcessingSlow,      // PROCESSING SLOW
    RecordAddition,      // RECORD ADDITION
    ConflictAmbiguous,   // CONFLICT-AMBIGUOUS
    ConflictSchedule,    // CONFLICT-SCHEDULE
    Review,              // REVIEW
    ReadOnlyResult,      // READ-ONLY RESULT
    Executing,           // EXECUTING
    ExecutionUncertain,  // EXECUTION UNCERTAIN
    ReconcileStatus,     // RECONCILE STATUS
    Success,             // SUCCESS
    Failure,             // FAILURE
    Cancelled,           // CANCELLED
    Count_
};

constexpr uint8_t kStateCount = static_cast<uint8_t>(State::Count_);

const char* name(State s);

// True while the device is doing work that must never time out into WALLPAPER
// (global rule 6: only READY goes to WALLPAPER).
bool is_active_work(State s);

// The three states the 60 s expiry applies to (team addition, 2026-10-09).
bool is_decision_state(State s);

}  // namespace halo
