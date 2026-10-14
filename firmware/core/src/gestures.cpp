#include "halo/gestures.hpp"

namespace halo {

void GestureDetector::emit(EventKind k, uint32_t at_ms, int32_t value) {
    uint8_t next_tail = static_cast<uint8_t>((tail_ + 1) % kQueue);
    if (next_tail == head_) return;  // queue full: the caller is not draining
    queue_[tail_] = Event::at(k, at_ms, value);
    tail_ = next_tail;
}

bool GestureDetector::next(Event& out) {
    if (head_ == tail_) return false;
    out = queue_[head_];
    head_ = static_cast<uint8_t>((head_ + 1) % kQueue);
    return true;
}

void GestureDetector::button_down(uint32_t at_ms) {
    if (had_edge_ && at_ms - last_edge_ < cfg_.debounce_ms) return;  // contact bounce
    last_edge_ = at_ms;
    had_edge_ = true;

    switch (phase_) {
        case Phase::PendingSingle:
            // Second press inside the window. Fired on the down edge, not on the
            // release: a double press cancels, and cancellation has to be immediate
            // (spec global rule 3). The consequence is that a press followed by a
            // hold reads as a double press, never as approve plus record. That is
            // the safe direction, since only a single press can approve.
            emit(EventKind::DoublePress, at_ms);
            phase_ = Phase::Swallow;
            return;
        case Phase::Idle:
            down_at_ = at_ms;
            phase_ = Phase::Down;
            return;
        default:
            return;  // already down: ignore
    }
}

void GestureDetector::button_up(uint32_t at_ms) {
    if (had_edge_ && at_ms - last_edge_ < cfg_.debounce_ms) return;
    last_edge_ = at_ms;
    had_edge_ = true;

    switch (phase_) {
        case Phase::Held:
            emit(EventKind::Release, at_ms);
            phase_ = Phase::Idle;
            return;
        case Phase::Down:
            // Short press. It cannot be reported yet: the next press decides
            // whether it was a single or the first half of a double.
            released_at_ = at_ms;
            phase_ = Phase::PendingSingle;
            return;
        case Phase::Swallow:
            phase_ = Phase::Idle;  // the release that belongs to the double press
            return;
        default:
            return;
    }
}

void GestureDetector::rotate(int32_t steps, uint32_t at_ms) {
    while (steps > 0) {
        emit(EventKind::RotateUp, at_ms, 1);
        --steps;
    }
    while (steps < 0) {
        emit(EventKind::RotateDown, at_ms, -1);
        ++steps;
    }
}

void GestureDetector::tick(uint32_t at_ms) {
    if (phase_ == Phase::Down && at_ms - down_at_ >= cfg_.hold_ms) {
        emit(EventKind::HoldStart, at_ms);
        phase_ = Phase::Held;
        return;
    }
    if (phase_ == Phase::PendingSingle && at_ms - released_at_ >= cfg_.double_press_ms) {
        emit(EventKind::SinglePress, at_ms);
        phase_ = Phase::Idle;
    }
}

}  // namespace halo
