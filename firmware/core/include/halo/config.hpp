// Every timing constant the device uses, in one place.
//
// PROVISIONAL means the number is a starting guess that has to be replaced by a
// measurement on the final board (Waveshare ESP32-S3 e-Paper 3.97). Numbers
// measured on any other board are provisional too, and are tagged with the board
// they came from in docs/CLAUDE.md.
//
// Nothing in core/ may include a board header. These are plain integers so the
// same values drive the host tests and the firmware.
#pragma once

#include <cstdint>

namespace halo {

struct Config {
    // ---- Gestures (side button) ----
    // A press longer than this is a hold, not a press. BRIEFING section 2.
    uint32_t hold_ms = 500;  // PROVISIONAL
    // Two presses closer together than this are a double press.
    uint32_t double_press_ms = 400;  // PROVISIONAL
    // Contact bounce on the tactile button is ignored for this long after an edge.
    uint32_t debounce_ms = 20;  // PROVISIONAL, depends on the button part

    // ---- Team additions (decided 2026-10-09) ----
    // Single presses are ignored for this long after the REVIEW screen finishes
    // drawing. Started by Event::ScreenPresented, never by a queue timestamp.
    uint32_t confirm_guard_ms = 1000;
    // REVIEW, CONFLICT-AMBIGUOUS and CONFLICT-SCHEDULE cancel this long after the
    // last interaction.
    uint32_t decision_expiry_ms = 60000;

    // ---- V1 spec numbers (the only two the spec fixes) ----
    // READY goes to WALLPAPER after more than one minute of inactivity.
    uint32_t wallpaper_idle_ms = 60000;
    // CRITICAL BATTERY at or below this, left only above it.
    uint8_t critical_battery_pct = 5;

    // ---- Deadlines the spec leaves open ----
    // Device side fallback only: the backend also sends its own slow event.
    uint32_t processing_slow_ms = 3000;        // PROVISIONAL
    uint32_t processing_deadline_ms = 20000;   // PROVISIONAL
    uint32_t delivery_deadline_ms = 10000;     // PROVISIONAL, FINALIZE AUDIO
    uint32_t execution_timeout_ms = 30000;     // PROVISIONAL, EXECUTING
    uint32_t outcome_timeout_ms = 5000;        // PROVISIONAL, SUCCESS and FAILURE
    uint32_t read_only_timeout_ms = 15000;     // PROVISIONAL, READ-ONLY RESULT
    uint32_t cancelled_notice_ms = 1500;       // PROVISIONAL, CANCELLED notice

    // ---- BLE pacing ----
    // Knob rotation reaches the phone as `activity` at most this often.
    uint32_t activity_rate_limit_ms = 10000;
};

}  // namespace halo
