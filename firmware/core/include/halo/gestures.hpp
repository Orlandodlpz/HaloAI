// Raw button and knob edges in, V1 gestures out.
//
// The side button carries single press, double press and hold. The rotary switch
// only rotates and wakes (BRIEFING.md section 2).
//
// Two rules decide the shape of this detector:
//  - A hold suppresses the click on release (spec global rule 7), so a hold never
//    also produces a press.
//  - A single press can only be acted on once the double press window has closed,
//    which adds that window to every confirm. That cost is already recorded as a
//    known gotcha in docs/CLAUDE.md.
#pragma once

#include "halo/config.hpp"
#include "halo/events.hpp"

namespace halo {

class GestureDetector {
public:
    explicit GestureDetector(const Config& cfg) : cfg_(cfg) {}

    // Raw edges from the hardware layer. Timestamps are monotonic milliseconds.
    void button_down(uint32_t at_ms);
    void button_up(uint32_t at_ms);
    // steps > 0 is up, steps < 0 is down. One event per detent.
    void rotate(int32_t steps, uint32_t at_ms);

    // Must be called regularly (at least every debounce_ms) or HoldStart and
    // SinglePress cannot fire: both are decided by time passing, not by an edge.
    void tick(uint32_t at_ms);

    // Drains one gesture. Returns false when there is nothing left.
    bool next(Event& out);

    bool button_is_down() const { return phase_ == Phase::Down || phase_ == Phase::Held || phase_ == Phase::Swallow; }

private:
    enum class Phase : uint8_t {
        Idle,
        Down,          // pressed, hold threshold not reached yet
        Held,          // hold threshold passed, HoldStart already emitted
        PendingSingle, // a short press ended, waiting out the double press window
        Swallow,       // a double press was emitted, ignore the release
    };

    void emit(EventKind k, uint32_t at_ms, int32_t value = 0);

    const Config& cfg_;
    Phase phase_ = Phase::Idle;
    uint32_t down_at_ = 0;
    uint32_t released_at_ = 0;
    uint32_t last_edge_ = 0;
    bool had_edge_ = false;

    static constexpr uint8_t kQueue = 8;
    Event queue_[kQueue]{};
    uint8_t head_ = 0;
    uint8_t tail_ = 0;
};

}  // namespace halo
