// Every transition in the V1 table and in the team additions table
// (BRIEFING.md section 2), plus the battery override from every state.
//
// Each test names the row it covers so a failure points back at the spec.
#include "harness.hpp"
#include "rig.hpp"

using namespace halo;
using namespace halo::test;
using E = halo::EventKind;

// ---------------------------------------------------------------- BOOT / RECOVERY

HALO_TEST(boot_startup_ok_goes_ready) {
    Rig r;
    auto t = r.send(E::StartupOk);
    CHECK_STATE(r.sm, State::Ready);
    CHECK(t.actions.has(Action::DrawScreen));
}

HALO_TEST(boot_pending_execution_goes_reconcile_status) {
    Rig r;
    auto t = r.send(E::PendingExecutionFound);
    CHECK_STATE(r.sm, State::ReconcileStatus);
    CHECK(t.actions.has(Action::SendStatusQuery));
    CHECK(r.sm.has_pending_execution());
}

// ------------------------------------------------------------------------- READY

HALO_TEST(ready_hold_begins_new_request) {
    Rig r;
    drive_to(r, State::Ready);
    r.send(E::HoldStart);
    CHECK_STATE(r.sm, State::PhoneCheck);
}

HALO_TEST(ready_inactivity_over_one_minute_goes_wallpaper) {
    Rig r;
    drive_to(r, State::Ready);
    r.advance(r.cfg.wallpaper_idle_ms);  // exactly one minute is not "more than"
    CHECK_STATE(r.sm, State::Ready);
    r.advance(2);
    CHECK_STATE(r.sm, State::Wallpaper);
}

HALO_TEST(ready_input_resets_the_wallpaper_timer) {
    Rig r;
    drive_to(r, State::Ready);
    r.advance(r.cfg.wallpaper_idle_ms - 100);
    r.send(E::RotateUp);
    r.advance(200);  // past the original deadline, not past the new one
    CHECK_STATE(r.sm, State::Ready);
    r.advance(r.cfg.wallpaper_idle_ms + 1);
    CHECK_STATE(r.sm, State::Wallpaper);
}

HALO_TEST(active_work_never_times_out_into_wallpaper) {
    for (uint8_t s = 0; s < kStateCount; ++s) {
        const State st = static_cast<State>(s);
        if (!is_active_work(st)) continue;
        Rig r;
        drive_to(r, st);
        if (r.sm.state() != st) continue;
        r.advance(r.cfg.wallpaper_idle_ms * 3);
        CHECK(r.sm.state() != State::Wallpaper);
    }
}

// --------------------------------------------------------------------- WALLPAPER

HALO_TEST(wallpaper_press_wakes_and_consumes_the_input) {
    Rig r;
    drive_to(r, State::Wallpaper);
    auto t = r.send(E::SinglePress);
    CHECK_STATE(r.sm, State::Ready);
    CHECK(t.note != nullptr);
}

HALO_TEST(wallpaper_knob_press_wakes_and_can_never_approve) {
    // The rotary knob press is a wake input, not one of the five gestures. It has
    // its own event kind so no code path can confuse it with the single press
    // that approves. The approve sweep covers it automatically.
    Rig r;
    drive_to(r, State::Wallpaper);
    auto t = r.send(E::Wake);
    CHECK_STATE(r.sm, State::Ready);
    CHECK(!t.actions.has(Action::SendApprove));
}

HALO_TEST(wallpaper_rotate_wakes) {
    Rig r;
    drive_to(r, State::Wallpaper);
    r.send(E::RotateDown);
    CHECK_STATE(r.sm, State::Ready);
}

HALO_TEST(wallpaper_hold_wakes_without_starting_a_recording) {
    // The wake input is consumed, so a hold in WALLPAPER does not reach
    // PHONE / BLUETOOTH CHECK. Interpretation of "consume wake input", flagged
    // in docs/CLAUDE.md for the team.
    Rig r;
    drive_to(r, State::Wallpaper);
    auto t = r.send(E::HoldStart);
    CHECK_STATE(r.sm, State::Ready);
    CHECK(!t.actions.has(Action::StartCapture));
}

// -------------------------------------------------------- PHONE / BLUETOOTH CHECK

HALO_TEST(phone_check_ready_starts_recording) {
    Rig r;
    drive_to(r, State::PhoneCheck);
    auto t = r.send(E::PhoneReady);
    CHECK_STATE(r.sm, State::Recording);
    CHECK(t.actions.has(Action::StartCapture));
}

HALO_TEST(phone_check_absent_goes_phone_disconnected) {
    Rig r;
    drive_to(r, State::PhoneCheck);
    r.send(E::PhoneAbsent);
    CHECK_STATE(r.sm, State::PhoneDisconnected);
}

HALO_TEST(phone_check_double_press_cancels) {
    Rig r;
    drive_to(r, State::PhoneCheck);
    r.send(E::DoublePress);
    CHECK_STATE(r.sm, State::Cancelled);
}

HALO_TEST(phone_check_release_before_the_phone_answers_cancels) {
    // Not in the V1 table: the user let go before capture started. Nothing was
    // recorded and nothing was sent, so it cancels. Flagged for the team.
    Rig r;
    drive_to(r, State::PhoneCheck);
    r.send(E::Release);
    CHECK_STATE(r.sm, State::Cancelled);
}

// ------------------------------------------------------------ PHONE DISCONNECTED

HALO_TEST(phone_disconnected_any_input_acknowledges) {
    const E inputs[] = {E::SinglePress, E::HoldStart, E::Release, E::RotateUp, E::RotateDown};
    for (E k : inputs) {
        Rig r;
        drive_to(r, State::PhoneDisconnected);
        r.send(k);
        CHECK_STATE(r.sm, State::Ready);
    }
}

HALO_TEST(phone_disconnected_double_press_cancels) {
    Rig r;
    drive_to(r, State::PhoneDisconnected);
    r.send(E::DoublePress);
    CHECK_STATE(r.sm, State::Cancelled);
}

// --------------------------------------------------------------------- RECORDING

HALO_TEST(recording_release_closes_the_segment) {
    Rig r;
    drive_to(r, State::Recording);
    auto t = r.send(E::Release);
    CHECK_STATE(r.sm, State::FinalizeAudio);
    CHECK(t.actions.has(Action::StopCapture));
    CHECK(t.actions.has(Action::SendFinalMarker));
}

HALO_TEST(recording_link_loss_or_buffer_exhaustion_fails) {
    Rig r;
    drive_to(r, State::Recording);
    auto t = r.send(E::TransportFailure);
    CHECK_STATE(r.sm, State::Failure);
    CHECK(t.actions.has(Action::StopCapture));
}

HALO_TEST(recording_double_press_cancels_and_stops_capture) {
    Rig r;
    drive_to(r, State::Recording);
    auto t = r.send(E::DoublePress);
    CHECK_STATE(r.sm, State::Cancelled);
    CHECK(t.actions.has(Action::StopCapture));
    CHECK(t.actions.has(Action::SendCancel));
}

// ---------------------------------------------------------------- FINALIZE AUDIO

HALO_TEST(finalize_all_acknowledged_goes_processing) {
    Rig r;
    drive_to(r, State::FinalizeAudio);
    r.send(E::AudioComplete);
    CHECK_STATE(r.sm, State::Processing);
}

HALO_TEST(finalize_delivery_deadline_fails) {
    Rig r;
    drive_to(r, State::FinalizeAudio);
    r.advance(r.cfg.delivery_deadline_ms + 1);
    CHECK_STATE(r.sm, State::Failure);
}

HALO_TEST(finalize_transport_failure_fails) {
    Rig r;
    drive_to(r, State::FinalizeAudio);
    r.send(E::TransportFailure);
    CHECK_STATE(r.sm, State::Failure);
}

HALO_TEST(finalize_double_press_cancels) {
    Rig r;
    drive_to(r, State::FinalizeAudio);
    r.send(E::DoublePress);
    CHECK_STATE(r.sm, State::Cancelled);
}

// --------------------------------------------------- PROCESSING / PROCESSING SLOW

HALO_TEST(processing_passes_the_slow_threshold) {
    Rig r;
    drive_to(r, State::Processing);
    r.advance(r.cfg.processing_slow_ms + 1);
    CHECK_STATE(r.sm, State::ProcessingSlow);
}

HALO_TEST(processing_backend_slow_event_also_moves_to_slow) {
    Rig r;
    drive_to(r, State::Processing);
    r.send(E::ProcessingSlow);
    CHECK_STATE(r.sm, State::ProcessingSlow);
}

HALO_TEST(slow_progress_resumes_to_processing) {
    Rig r;
    drive_to(r, State::ProcessingSlow);
    r.send(E::ProcessingProgress);
    CHECK_STATE(r.sm, State::Processing);
}

HALO_TEST(processing_deadline_is_not_extended_by_going_slow) {
    Rig r;
    drive_to(r, State::Processing);
    const uint32_t deadline = r.sm.deadline_at();
    r.send(E::ProcessingSlow);
    CHECK(r.sm.deadline_at() == deadline);
    r.send(E::ProcessingProgress);
    CHECK(r.sm.deadline_at() == deadline);
}

HALO_TEST(processing_deadline_fails) {
    // The slow threshold comes first in time, so the request passes through
    // PROCESSING SLOW on its way to the deadline. That is the real sequence.
    Rig r;
    drive_to(r, State::Processing);
    r.advance(r.cfg.processing_slow_ms + 1);
    CHECK_STATE(r.sm, State::ProcessingSlow);
    r.advance(r.cfg.processing_deadline_ms);
    CHECK_STATE(r.sm, State::Failure);
}

HALO_TEST(slow_deadline_fails) {
    Rig r;
    drive_to(r, State::ProcessingSlow);
    r.advance(r.cfg.processing_deadline_ms + 1);
    CHECK_STATE(r.sm, State::Failure);
}

HALO_TEST(processing_error_fails) {
    for (State st : {State::Processing, State::ProcessingSlow}) {
        Rig r;
        drive_to(r, st);
        r.send(E::ProcessingFailed);
        CHECK_STATE(r.sm, State::Failure);
    }
}

HALO_TEST(processing_hold_records_an_addition) {
    for (State st : {State::Processing, State::ProcessingSlow}) {
        Rig r;
        drive_to(r, st);
        auto t = r.send(E::HoldStart);
        CHECK_STATE(r.sm, State::RecordAddition);
        CHECK(t.actions.has(Action::StartCapture));
    }
}

HALO_TEST(processing_double_press_cancels) {
    for (State st : {State::Processing, State::ProcessingSlow}) {
        Rig r;
        drive_to(r, st);
        auto t = r.send(E::DoublePress);
        CHECK_STATE(r.sm, State::Cancelled);
        CHECK(t.actions.has(Action::SendCancel));
    }
}

HALO_TEST(processing_results_route_to_their_states) {
    struct Row { E event; State to; };
    const Row rows[] = {
        {E::ScreenConflictAmbiguous, State::ConflictAmbiguous},
        {E::ScreenConflictSchedule, State::ConflictSchedule},
        {E::ScreenReview, State::Review},
        {E::ScreenReadOnly, State::ReadOnlyResult},
    };
    for (State from : {State::Processing, State::ProcessingSlow}) {
        for (const Row& row : rows) {
            Rig r;
            drive_to(r, from);
            r.send(row.event);
            CHECK_STATE(r.sm, row.to);
        }
    }
}

// --------------------------------------------------------------- RECORD ADDITION

HALO_TEST(addition_release_finalizes) {
    Rig r;
    drive_to(r, State::RecordAddition);
    auto t = r.send(E::Release);
    CHECK_STATE(r.sm, State::FinalizeAudio);
    CHECK(t.actions.has(Action::SendFinalMarker));
}

HALO_TEST(addition_transport_failure_fails) {
    Rig r;
    drive_to(r, State::RecordAddition);
    r.send(E::TransportFailure);
    CHECK_STATE(r.sm, State::Failure);
}

HALO_TEST(addition_double_press_cancels) {
    Rig r;
    drive_to(r, State::RecordAddition);
    r.send(E::DoublePress);
    CHECK_STATE(r.sm, State::Cancelled);
}

// ------------------------------------------------------------ CONFLICT-AMBIGUOUS

HALO_TEST(ambiguous_rotate_moves_the_highlight_and_reports_activity) {
    Rig r;
    drive_to(r, State::ConflictAmbiguous);
    auto t = r.send(E::RotateUp);
    CHECK_STATE(r.sm, State::ConflictAmbiguous);
    CHECK(t.actions.has(Action::SendActivity));
}

HALO_TEST(ambiguous_activity_is_rate_limited_to_one_per_ten_seconds) {
    Rig r;
    drive_to(r, State::ConflictAmbiguous);
    CHECK(r.send(E::RotateUp).actions.has(Action::SendActivity));
    CHECK(!r.send(E::RotateUp).actions.has(Action::SendActivity));
    r.now += r.cfg.activity_rate_limit_ms;
    CHECK(r.send(E::RotateDown).actions.has(Action::SendActivity));
}

HALO_TEST(ambiguous_rotate_resets_the_sixty_second_expiry) {
    Rig r;
    drive_to(r, State::ConflictAmbiguous);
    r.advance(r.cfg.decision_expiry_ms - 100);
    r.send(E::RotateUp);
    r.advance(200);
    CHECK_STATE(r.sm, State::ConflictAmbiguous);
}

HALO_TEST(ambiguous_single_press_selects_the_candidate) {
    Rig r;
    drive_to(r, State::ConflictAmbiguous);
    auto t = r.send(E::SinglePress);
    CHECK_STATE(r.sm, State::Processing);
    CHECK(t.actions.has(Action::SendSelect));
}

HALO_TEST(ambiguous_hold_answers_by_voice) {
    Rig r;
    drive_to(r, State::ConflictAmbiguous);
    r.send(E::HoldStart);
    CHECK_STATE(r.sm, State::RecordAddition);
}

HALO_TEST(ambiguous_double_press_cancels) {
    Rig r;
    drive_to(r, State::ConflictAmbiguous);
    r.send(E::DoublePress);
    CHECK_STATE(r.sm, State::Cancelled);
}

// ------------------------------------------------------------- CONFLICT-SCHEDULE

HALO_TEST(schedule_single_press_continues_to_review) {
    Rig r;
    drive_to(r, State::ConflictSchedule);
    auto t = r.send(E::SinglePress);
    CHECK_STATE(r.sm, State::Review);
    CHECK(t.actions.has(Action::SendContinue));
    CHECK(!t.actions.has(Action::SendApprove));
}

HALO_TEST(schedule_hold_changes_details_by_voice) {
    Rig r;
    drive_to(r, State::ConflictSchedule);
    r.send(E::HoldStart);
    CHECK_STATE(r.sm, State::RecordAddition);
}

HALO_TEST(schedule_double_press_cancels) {
    Rig r;
    drive_to(r, State::ConflictSchedule);
    r.send(E::DoublePress);
    CHECK_STATE(r.sm, State::Cancelled);
}

// --------------------------------------------------------- 60 s expiry, all three

HALO_TEST(decision_states_expire_after_sixty_seconds) {
    for (State st : {State::ConflictAmbiguous, State::ConflictSchedule, State::Review}) {
        Rig r;
        drive_to(r, st);
        r.advance(r.cfg.decision_expiry_ms - 10);
        CHECK_STATE(r.sm, st);
        auto t = r.advance(20);
        CHECK_STATE(r.sm, State::Cancelled);
        CHECK(t.actions.has(Action::SendCancel));
    }
}

// ------------------------------------------------------------------------ REVIEW

HALO_TEST(review_press_before_the_screen_is_presented_is_ignored) {
    Rig r;
    drive_to(r, State::Review);
    auto t = r.send(E::SinglePress);
    CHECK_STATE(r.sm, State::Review);
    CHECK(!t.actions.has(Action::SendApprove));
}

HALO_TEST(review_press_within_the_guard_is_ignored) {
    Rig r;
    drive_to(r, State::Review);
    r.send(E::ScreenPresented);
    r.now += r.cfg.confirm_guard_ms - 50;
    auto t = r.sm.handle(Event::at(E::SinglePress, r.now));
    CHECK_STATE(r.sm, State::Review);
    CHECK(!t.actions.has(Action::SendApprove));
}

HALO_TEST(review_single_press_after_the_guard_approves) {
    Rig r;
    drive_to(r, State::Review);
    r.send(E::ScreenPresented);
    r.now += r.cfg.confirm_guard_ms + 1;
    auto t = r.sm.handle(Event::at(E::SinglePress, r.now));
    CHECK_STATE(r.sm, State::Executing);
    CHECK(t.actions.has(Action::SendApprove));
    CHECK(t.actions.has(Action::PersistExecutionKey));
    CHECK(r.sm.has_pending_execution());
}

HALO_TEST(review_guard_runs_from_the_present_call_not_from_entering_review) {
    // The whole point of wiring the guard to present(): on e-paper the screen
    // appears hundreds of ms after the state changed. Time spent waiting for the
    // panel must not count towards the guard.
    Rig r;
    drive_to(r, State::Review);
    r.advance(5000);                  // 5 s of drawing, screen not on the panel yet
    r.send(E::ScreenPresented);       // pixels are there now, guard starts here
    r.now += r.cfg.confirm_guard_ms - 50;
    auto t = r.sm.handle(Event::at(E::SinglePress, r.now));
    CHECK(!t.actions.has(Action::SendApprove));
    CHECK_STATE(r.sm, State::Review);
}

HALO_TEST(review_rotate_scrolls_and_resets_expiry) {
    Rig r;
    drive_to(r, State::Review);
    r.advance(r.cfg.decision_expiry_ms - 100);
    r.send(E::RotateDown);
    CHECK_STATE(r.sm, State::Review);
    r.advance(200);
    CHECK_STATE(r.sm, State::Review);
}

HALO_TEST(review_hold_revises_by_voice) {
    Rig r;
    drive_to(r, State::Review);
    r.send(E::HoldStart);
    CHECK_STATE(r.sm, State::RecordAddition);
}

HALO_TEST(review_double_press_cancels) {
    Rig r;
    drive_to(r, State::Review);
    r.send(E::ScreenPresented);
    r.advance(r.cfg.confirm_guard_ms + 1);
    auto t = r.send(E::DoublePress);
    CHECK_STATE(r.sm, State::Cancelled);
    CHECK(t.actions.has(Action::SendCancel));
    CHECK(!t.actions.has(Action::SendApprove));
}

// -------------------------------------------------------------- READ-ONLY RESULT

HALO_TEST(read_only_rotate_scrolls) {
    Rig r;
    drive_to(r, State::ReadOnlyResult);
    r.send(E::RotateUp);
    CHECK_STATE(r.sm, State::ReadOnlyResult);
}

HALO_TEST(read_only_single_press_returns_to_ready) {
    Rig r;
    drive_to(r, State::ReadOnlyResult);
    auto t = r.send(E::SinglePress);
    CHECK_STATE(r.sm, State::Ready);
    CHECK(!t.actions.has(Action::SendApprove));
}

HALO_TEST(read_only_result_times_out_to_ready) {
    Rig r;
    drive_to(r, State::ReadOnlyResult);
    r.advance(r.cfg.read_only_timeout_ms + 1);
    CHECK_STATE(r.sm, State::Ready);
}

// --------------------------------------------------------------------- EXECUTING

HALO_TEST(executing_authoritative_success) {
    Rig r;
    drive_to(r, State::Executing);
    auto t = r.send(E::ExecutionApplied);
    CHECK_STATE(r.sm, State::Success);
    CHECK(t.actions.has(Action::ClearExecutionKey));
    CHECK(!r.sm.has_pending_execution());
}

HALO_TEST(executing_authoritative_rejection) {
    Rig r;
    drive_to(r, State::Executing);
    auto t = r.send(E::ExecutionRejected);
    CHECK_STATE(r.sm, State::Failure);
    CHECK(t.actions.has(Action::ClearExecutionKey));
    CHECK(!r.sm.has_pending_execution());
}

HALO_TEST(executing_ambiguous_response_is_uncertain) {
    Rig r;
    drive_to(r, State::Executing);
    r.send(E::ExecutionUnknown);
    CHECK_STATE(r.sm, State::ExecutionUncertain);
}

HALO_TEST(executing_timeout_is_uncertain) {
    Rig r;
    drive_to(r, State::Executing);
    r.advance(r.cfg.execution_timeout_ms + 1);
    CHECK_STATE(r.sm, State::ExecutionUncertain);
}

HALO_TEST(executing_link_loss_is_uncertain) {
    Rig r;
    drive_to(r, State::Executing);
    r.send(E::TransportFailure);
    CHECK_STATE(r.sm, State::ExecutionUncertain);
}

HALO_TEST(executing_double_press_only_stops_waiting) {
    // The effect may already exist, so the pending key is kept.
    Rig r;
    drive_to(r, State::Executing);
    auto t = r.send(E::DoublePress);
    CHECK_STATE(r.sm, State::ExecutionUncertain);
    CHECK(!t.actions.has(Action::SendCancel));
    CHECK(!t.actions.has(Action::ClearExecutionKey));
    CHECK(r.sm.has_pending_execution());
}

// ------------------------------------------------------------ EXECUTION UNCERTAIN

HALO_TEST(uncertain_single_press_reconciles) {
    Rig r;
    drive_to(r, State::ExecutionUncertain);
    auto t = r.send(E::SinglePress);
    CHECK_STATE(r.sm, State::ReconcileStatus);
    CHECK(t.actions.has(Action::SendStatusQuery));
    CHECK(!t.actions.has(Action::SendApprove));
}

HALO_TEST(uncertain_background_status_query_reconciles) {
    Rig r;
    drive_to(r, State::ExecutionUncertain);
    r.send(E::StatusQueryDue);
    CHECK_STATE(r.sm, State::ReconcileStatus);
}

HALO_TEST(uncertain_double_press_dismisses_and_keeps_the_action_saved) {
    Rig r;
    drive_to(r, State::ExecutionUncertain);
    auto t = r.send(E::DoublePress);
    CHECK_STATE(r.sm, State::Ready);
    CHECK(!t.actions.has(Action::ClearExecutionKey));
    CHECK(r.sm.has_pending_execution());
}

// -------------------------------------------------------------- RECONCILE STATUS

HALO_TEST(reconcile_confirmed_applied) {
    Rig r;
    drive_to(r, State::ReconcileStatus);
    auto t = r.send(E::ExecutionApplied);
    CHECK_STATE(r.sm, State::Success);
    CHECK(t.actions.has(Action::ClearExecutionKey));
}

HALO_TEST(reconcile_confirmed_not_applied) {
    Rig r;
    drive_to(r, State::ReconcileStatus);
    r.send(E::ExecutionRejected);
    CHECK_STATE(r.sm, State::Failure);
}

HALO_TEST(reconcile_still_inconclusive) {
    Rig r;
    drive_to(r, State::ReconcileStatus);
    r.send(E::ExecutionUnknown);
    CHECK_STATE(r.sm, State::ExecutionUncertain);
}

HALO_TEST(reconcile_timeout_is_uncertain) {
    Rig r;
    drive_to(r, State::ReconcileStatus);
    r.advance(r.cfg.execution_timeout_ms + 1);
    CHECK_STATE(r.sm, State::ExecutionUncertain);
}

// -------------------------------------------------- SUCCESS, FAILURE, CANCELLED

HALO_TEST(success_and_failure_take_any_input_as_acknowledgment) {
    const E inputs[] = {E::SinglePress, E::DoublePress, E::HoldStart, E::Release,
                        E::RotateUp, E::RotateDown};
    for (State st : {State::Success, State::Failure}) {
        for (E k : inputs) {
            Rig r;
            drive_to(r, st);
            r.send(k);
            CHECK_STATE(r.sm, State::Ready);
        }
    }
}

HALO_TEST(success_and_failure_time_out_to_ready) {
    for (State st : {State::Success, State::Failure}) {
        Rig r;
        drive_to(r, st);
        r.advance(r.cfg.outcome_timeout_ms + 1);
        CHECK_STATE(r.sm, State::Ready);
    }
}

HALO_TEST(cancelled_notice_completes_to_ready) {
    Rig r;
    drive_to(r, State::Cancelled);
    r.send(E::CancelAck);
    CHECK_STATE(r.sm, State::Ready);
}

HALO_TEST(cancelled_notice_times_out_to_ready) {
    Rig r;
    drive_to(r, State::Cancelled);
    r.advance(r.cfg.cancelled_notice_ms + 1);
    CHECK_STATE(r.sm, State::Ready);
}

// -------------------------------------------------------------- CRITICAL BATTERY

HALO_TEST(critical_battery_is_entered_from_every_state) {
    for (uint8_t s = 0; s < kStateCount; ++s) {
        const State st = static_cast<State>(s);
        if (st == State::CriticalBattery) continue;
        Rig r;
        drive_to(r, st);
        if (r.sm.state() != st) continue;  // state not reachable by the driver
        auto t = r.send(E::BatteryLevel, 5);
        CHECK_STATE(r.sm, State::CriticalBattery);
        CHECK(t.actions.has(Action::InhibitCapture));
    }
}

HALO_TEST(critical_battery_stops_an_open_capture) {
    for (State st : {State::Recording, State::RecordAddition}) {
        Rig r;
        drive_to(r, st);
        auto t = r.send(E::BatteryLevel, 4);
        CHECK_STATE(r.sm, State::CriticalBattery);
        CHECK(t.actions.has(Action::StopCapture));
    }
}

HALO_TEST(critical_battery_blocks_every_other_event) {
    Rig r;
    drive_to(r, State::CriticalBattery);
    const E all[] = {E::SinglePress, E::DoublePress, E::HoldStart, E::Release, E::RotateUp,
                     E::ScreenReview, E::ScreenPresented, E::AudioComplete, E::ExecutionApplied};
    for (E k : all) {
        auto t = r.send(k);
        CHECK_STATE(r.sm, State::CriticalBattery);
        CHECK(!t.handled);
        CHECK(t.actions.count == 0);
    }
}

HALO_TEST(critical_battery_is_left_only_above_the_threshold) {
    Rig r;
    drive_to(r, State::CriticalBattery);
    r.send(E::BatteryLevel, r.cfg.critical_battery_pct);  // exactly at the threshold
    CHECK_STATE(r.sm, State::CriticalBattery);
    r.send(E::BatteryLevel, r.cfg.critical_battery_pct + 1);
    CHECK_STATE(r.sm, State::Ready);
}

HALO_TEST(critical_battery_reconciles_a_pending_execution_before_ready) {
    Rig r;
    drive_to(r, State::Executing);
    r.send(E::BatteryLevel, 3);
    CHECK_STATE(r.sm, State::CriticalBattery);
    CHECK(r.sm.has_pending_execution());
    auto t = r.send(E::BatteryLevel, 20);
    CHECK_STATE(r.sm, State::ReconcileStatus);
    CHECK(t.actions.has(Action::SendStatusQuery));
}

// ------------------------------------------- the hard constraint, stated as a test

HALO_TEST(no_state_and_no_event_approves_while_the_confirm_guard_is_closed) {
    // The sweep never presents a REVIEW screen, so the guard is closed everywhere.
    // Nothing in the machine may produce SendApprove under those conditions.
    for (uint8_t s = 0; s < kStateCount; ++s) {
        for (uint8_t k = 0; k < static_cast<uint8_t>(E::Count_); ++k) {
            Rig r;
            drive_to(r, static_cast<State>(s));
            // value 50: a BatteryLevel event in this sweep must not read as critical.
            auto t = r.send(static_cast<E>(k), 50);
            CHECK(!t.actions.has(Action::SendApprove));
            CHECK(!t.actions.has(Action::PersistExecutionKey));
        }
    }
}

HALO_TEST(approve_is_reachable_so_the_sweep_is_not_vacuous) {
    Rig r;
    drive_to(r, State::Review);
    r.send(E::ScreenPresented);
    r.now += r.cfg.confirm_guard_ms + 1;
    CHECK(r.sm.handle(Event::at(E::SinglePress, r.now)).actions.has(Action::SendApprove));
}

HALO_TEST(every_state_is_reachable_by_the_test_driver) {
    for (uint8_t s = 0; s < kStateCount; ++s) {
        Rig r;
        drive_to(r, static_cast<State>(s));
        CHECK_STATE(r.sm, static_cast<State>(s));
    }
}

// ---------------------------------------------------------------------------
// The backend cancels a request on its own (team additions, 2026-10-17):
// Orlando's row for the ordinary case, Carlos's for the one that also has to
// throw away a saved execution key.

HALO_TEST(a_cancelled_screen_from_the_phone_cancels_work_in_flight) {
    // The request expired on the backend, or a newer one replaced it. Everything
    // before the approve is the backend's to cancel.
    for (State from : {State::PhoneCheck, State::FinalizeAudio, State::Processing,
                       State::ProcessingSlow, State::ConflictAmbiguous,
                       State::ConflictSchedule, State::Review}) {
        Rig r;
        drive_to(r, from);
        CHECK_STATE(r.sm, from);
        const Transition t = r.send(EventKind::ScreenCancelled);
        CHECK(t.handled);
        CHECK_STATE(r.sm, State::Cancelled);
        CHECK(!t.actions.has(Action::ClearExecutionKey));  // nothing was dispatched
    }
}

HALO_TEST(a_cancelled_screen_while_recording_stops_the_microphone) {
    for (State from : {State::Recording, State::RecordAddition}) {
        Rig r;
        drive_to(r, from);
        const Transition t = r.send(EventKind::ScreenCancelled);
        CHECK_STATE(r.sm, State::Cancelled);
        CHECK(t.actions.has(Action::StopCapture));
    }
}

HALO_TEST(a_cancelled_screen_in_reconcile_status_clears_the_key) {
    // Carlos's row. The device saved a key as it left REVIEW, the approve never
    // reached the backend, and the request was cancelled there. The status
    // endpoint says so, and the key is for something that never happened.
    Rig r;
    drive_to(r, State::ReconcileStatus);
    CHECK(r.sm.has_pending_execution());
    const Transition t = r.send(EventKind::ScreenCancelled);
    CHECK_STATE(r.sm, State::Cancelled);
    CHECK(t.actions.has(Action::ClearExecutionKey));
    CHECK(!r.sm.has_pending_execution());
}

HALO_TEST(a_cancelled_screen_never_throws_away_a_dispatched_action) {
    // The dangerous direction. Once approve has gone out the effect may already
    // exist, so a cancel screen must not clear the key or claim nothing happened.
    for (State from : {State::Executing, State::ExecutionUncertain}) {
        Rig r;
        drive_to(r, from);
        CHECK(r.sm.has_pending_execution());
        const Transition t = r.send(EventKind::ScreenCancelled);
        CHECK(!t.handled);
        CHECK_STATE(r.sm, from);
        CHECK(!t.actions.has(Action::ClearExecutionKey));
        CHECK(r.sm.has_pending_execution());
    }
}

HALO_TEST(a_cancelled_screen_is_ignored_when_nothing_is_in_flight) {
    for (State from : {State::Ready, State::Wallpaper}) {
        Rig r;
        drive_to(r, from);
        const Transition t = r.send(EventKind::ScreenCancelled);
        CHECK(!t.handled);
        CHECK_STATE(r.sm, from);
    }
}

HALO_TEST(any_input_dismisses_the_cancellation_notice) {
    // Team addition 2026-10-17: waiting out the notice timer is no longer the
    // only way out of CANCELLED.
    for (EventKind k : {EventKind::SinglePress, EventKind::DoublePress, EventKind::RotateUp,
                        EventKind::Wake, EventKind::HoldStart}) {
        Rig r;
        drive_to(r, State::Cancelled);
        const Transition t = r.send(k);
        CHECK(t.handled);
        CHECK_STATE(r.sm, State::Ready);
    }
}

HALO_TEST(an_authoritative_outcome_resolves_execution_uncertain) {
    // The phone polls the status endpoint while the device waits, so an answer
    // can arrive without the user asking for it. Section 2, 2026-10-19: the key
    // is cleared on an outcome in this state too. Ignoring one would leave a key
    // in flash for something the backend has already settled.
    {
        Rig r;
        drive_to(r, State::ExecutionUncertain);
        CHECK(r.sm.has_pending_execution());
        const Transition t = r.send(EventKind::ExecutionApplied);
        CHECK_STATE(r.sm, State::Success);
        CHECK(t.actions.has(Action::ClearExecutionKey));
        CHECK(!r.sm.has_pending_execution());
    }
    {
        Rig r;
        drive_to(r, State::ExecutionUncertain);
        const Transition t = r.send(EventKind::ExecutionRejected);
        CHECK_STATE(r.sm, State::Failure);
        CHECK(t.actions.has(Action::ClearExecutionKey));
        CHECK(!r.sm.has_pending_execution());
    }
    // Still dismissable, and dismissing still keeps the key.
    {
        Rig r;
        drive_to(r, State::ExecutionUncertain);
        const Transition t = r.send(EventKind::DoublePress);
        CHECK_STATE(r.sm, State::Ready);
        CHECK(!t.actions.has(Action::ClearExecutionKey));
        CHECK(r.sm.has_pending_execution());
    }
}
