// Everything that can move the state machine, and everything it asks the
// hardware layer to do in return.
//
// The state machine never reads a clock, a pin, a radio or a screen. It is given
// events that already carry their timestamp, and it answers with actions that the
// hardware layer carries out. That is what makes every transition testable on a
// computer.
#pragma once

#include <cstdint>

namespace halo {

enum class EventKind : uint8_t {
    // --- Gestures, from GestureDetector (side button and rotary switch) ---
    SinglePress,
    DoublePress,
    HoldStart,  // the hold threshold passed: start recording
    Release,    // the held button came up: close the voice segment
    RotateUp,
    RotateDown,

    // --- Local, from the device itself ---
    Tick,                   // time passed; drives every timeout
    BatteryLevel,           // value = percent remaining
    StartupOk,              // BOOT: checks passed and no pending execution in flash
    PendingExecutionFound,  // BOOT: an execution key was found in flash
    PhoneReady,             // phone linked and its capture service is ready
    PhoneAbsent,            // phone absent or service unavailable
    AudioComplete,          // every chunk and the final marker were acknowledged
    TransportFailure,       // link loss, capture error, buffer exhaustion, deadline
    ScreenPresented,        // the display backend's present() returned

    // --- From the phone (the backend owns these decisions) ---
    ScreenConflictAmbiguous,
    ScreenConflictSchedule,
    ScreenReview,
    ScreenReadOnly,
    ProcessingSlow,      // backend said processing passed the slow threshold
    ProcessingProgress,  // progress resumed, latest revision still active
    ProcessingFailed,    // STT or model error, or the processing deadline
    ExecutionApplied,    // authoritative success for the execution key
    ExecutionRejected,   // authoritative rejection, confirmed no effect
    ExecutionUnknown,    // ambiguous response, link loss or restart
    CancelAck,           // the cancellation notice finished
    StatusQueryDue,      // background status query, while an execution is uncertain
    Count_
};

struct Event {
    EventKind kind = EventKind::Tick;
    uint32_t at_ms = 0;   // monotonic milliseconds, supplied by the caller
    int32_t value = 0;    // battery percent, rotation steps; unused otherwise

    static Event at(EventKind k, uint32_t t, int32_t v = 0) { return Event{k, t, v}; }
};

const char* name(EventKind k);

// What the state machine asks the hardware layer to do. The order in the list is
// the order they must happen in.
enum class Action : uint8_t {
    DrawScreen,          // draw the screen of the state just entered
    StartCapture,        // open the microphone and start streaming chunks
    StopCapture,         // stop the microphone, drop the open segment
    SendFinalMarker,     // close the segment and wait for its acknowledgment
    InhibitCapture,      // critical battery: no new capture, no new execution
    PersistExecutionKey, // write the pending execution key to flash
    ClearExecutionKey,   // the outcome is known: remove it from flash
    SendApprove,         // approve the displayed revision (the only mutating path)
    SendCancel,
    SendSelect,          // the highlighted candidate
    SendContinue,        // continue despite a scheduling conflict
    SendStatusQuery,     // reconcile an uncertain execution
    SendActivity,        // knob activity, forwarded by the phone as `touch`
    ShowWallpaper,
    Count_
};

const char* name(Action a);

// Fixed size: no allocation on the device.
struct ActionList {
    static constexpr uint8_t kMax = 4;
    Action items[kMax]{};
    uint8_t count = 0;

    void add(Action a) {
        if (count < kMax) items[count++] = a;
    }
    bool has(Action a) const {
        for (uint8_t i = 0; i < count; ++i)
            if (items[i] == a) return true;
        return false;
    }
};

}  // namespace halo
