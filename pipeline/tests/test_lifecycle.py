"""Request lifecycle against a real Postgres, with fake processor, executor and clock.
Covers the B1 gate items that do not need HTTP."""

import asyncio

import pytest

from app.lifecycle import ApiError
from app.screens import fits
from tests.conftest import addition_body, start_body


async def new_review(lc, user, text="block 3 to 4 tomorrow for the gym", scope="real"):
    body = start_body(text)
    rev = await lc.start(user, scope, body)
    result = await lc.process(body["request_id"], rev)
    return body["request_id"], result


def approve_body(result):
    return {"revision": result["revision"], "proposal_hash": result["proposal_hash"]}


# ---------- revisions ----------

async def test_start_to_review_sets_expiry_and_hash(lc, user):
    _, r = await new_review(lc, user)
    assert r["state"] == "review" and r["revision"] == 1
    assert r["proposal_hash"] and r["draft"]
    assert r["expires_in_s"] == 60 and r["expires_at"]


async def test_addition_invalidates_old_proposal(lc, user):
    rid, r1 = await new_review(lc, user)
    rev2 = await lc.add(user, rid, addition_body("make it Thursday", 1))
    r2 = await lc.process(rid, rev2)
    assert r2["revision"] == 2 and r2["state"] == "review"
    with pytest.raises(ApiError) as e:
        await lc.approve(user, rid, approve_body(r1))
    assert e.value.code == "stale_revision" and e.value.current["revision"] == 2


async def test_addition_with_stale_base_is_appended_not_rejected(lc, user):
    rid, _ = await new_review(lc, user)
    await lc.add(user, rid, addition_body("first", 1))
    rev3 = await lc.add(user, rid, addition_body("second, same stale base", 1))
    assert rev3 == 3
    r = await lc.process(rid, rev3)
    assert r["revision"] == 3
    texts = [s.text for s in lc.processor.calls[-1].segments]
    assert texts == ["block 3 to 4 tomorrow for the gym", "first", "second, same stale base"]


async def test_latest_revision_wins_stale_result_dropped(lc, user, processor):
    body = start_body()
    await lc.start(user, "real", body)
    processor.gate = asyncio.Event()
    slow = asyncio.create_task(lc.process(body["request_id"], 1))  # revision 1 still thinking
    await asyncio.sleep(0.05)
    rev2 = await lc.add(user, body["request_id"], addition_body("actually 4 to 5", 1))
    processor.gate.set()
    stale = await slow
    fresh = await lc.process(body["request_id"], rev2)
    assert stale["revision"] == 1  # returned to the old stream, which the phone drops
    current = await lc.get_result(user, body["request_id"])
    assert current["revision"] == 2 == fresh["revision"]


async def test_late_result_for_cancelled_request_ignored(lc, user, processor):
    body = start_body()
    await lc.start(user, "real", body)
    processor.gate = asyncio.Event()
    running = asyncio.create_task(lc.process(body["request_id"], 1))
    await asyncio.sleep(0.05)
    await lc.cancel(user, body["request_id"])
    processor.gate.set()
    late = await running
    assert late["state"] == "cancelled"
    assert (await lc.get_result(user, body["request_id"]))["state"] == "cancelled"


# ---------- expiry and touch ----------

async def test_expiry_cancels_decision_state(lc, user, clock):
    rid, r = await new_review(lc, user)
    clock.advance(61)
    with pytest.raises(ApiError) as e:
        await lc.approve(user, rid, approve_body(r))
    assert e.value.code == "expired" and e.value.current["state"] == "cancelled"


async def test_touch_resets_expiry(lc, user, clock):
    rid, r = await new_review(lc, user)
    clock.advance(50)
    touched = await lc.touch(user, rid, r["revision"])
    assert touched["expires_in_s"] == 60
    clock.advance(50)  # 100 s after the result, 50 s after the touch
    result = await lc.approve(user, rid, approve_body(r))
    assert result["state"] == "success"


async def test_touch_stale_revision_rejected(lc, user):
    rid, _ = await new_review(lc, user)
    await lc.process(rid, await lc.add(user, rid, addition_body("change", 1)))
    with pytest.raises(ApiError) as e:
        await lc.touch(user, rid, 1)
    assert e.value.code == "stale_revision"


# ---------- approval exactly once ----------

async def test_wrong_hash_rejected(lc, user):
    rid, r = await new_review(lc, user)
    with pytest.raises(ApiError) as e:
        await lc.approve(user, rid, {"revision": 1, "proposal_hash": "0" * 64})
    assert e.value.code == "hash_mismatch"


async def test_repeated_approval_executes_once(lc, user, executors):
    rid, r = await new_review(lc, user)
    first = await lc.approve(user, rid, approve_body(r))
    second = await lc.approve(user, rid, approve_body(r))  # e.g. new call_id after app restart
    assert first["state"] == second["state"] == "success"
    assert first["execution_key"] == second["execution_key"] and len(first["execution_key"]) == 26
    assert len(executors["mock"].calls) == 1


async def test_concurrent_approvals_execute_once(lc, user, executors):
    rid, r = await new_review(lc, user)
    results = await asyncio.gather(*[lc.approve(user, rid, approve_body(r)) for _ in range(5)])
    await lc.drain()
    assert len(executors["mock"].calls) == 1
    assert {x["execution_key"] for x in results} == {results[0]["execution_key"]}


async def test_slow_execution_returns_executing_then_status_success(lc, user, executors):
    executors["mock"].delay_s = 0.35  # longer than approve_wait_s (0.2) in tests
    rid, r = await new_review(lc, user)
    result = await lc.approve(user, rid, approve_body(r))
    assert result["state"] == "executing" and result["execution_key"]
    await lc.drain()
    status = await lc.execution_status(user, result["execution_key"])
    assert status["state"] == "success"


@pytest.mark.parametrize("mode,hang,expected", [
    ("fail", False, "failure"),
    ("error", False, "execution_uncertain"),
    ("succeed", True, "execution_uncertain"),  # past the execution deadline: unknown
])
async def test_execution_outcomes(lc, user, executors, mode, hang, expected):
    executors["mock"].mode, executors["mock"].hang = mode, hang
    rid, r = await new_review(lc, user)
    result = await lc.approve(user, rid, approve_body(r))
    await lc.drain()
    status = await lc.execution_status(user, result["execution_key"])
    assert status["state"] == expected


async def test_unknown_execution_key(lc, user):
    with pytest.raises(ApiError) as e:
        await lc.execution_status(user, "nope")
    assert e.value.code == "not_found"


# ---------- cancel and one request at a time ----------

async def test_cancel_after_approve_rejected(lc, user):
    rid, r = await new_review(lc, user)
    await lc.approve(user, rid, approve_body(r))
    with pytest.raises(ApiError) as e:
        await lc.cancel(user, rid)
    assert e.value.code == "already_executing"


async def test_cancel_is_harmless_to_repeat_and_leaves_nothing(lc, user, executors):
    rid, _ = await new_review(lc, user)
    assert (await lc.cancel(user, rid))["state"] == "cancelled"
    assert (await lc.cancel(user, rid))["state"] == "cancelled"
    assert executors["mock"].calls == [] and executors["real"].calls == []


async def test_new_request_cancels_unapproved_old_one(lc, user):
    old_rid, _ = await new_review(lc, user)
    await new_review(lc, user, "another command")
    assert (await lc.get_result(user, old_rid))["state"] == "cancelled"


async def test_new_request_refused_while_old_one_executes(lc, user, executors):
    executors["mock"].delay_s = 0.4
    rid, r = await new_review(lc, user)
    await lc.approve(user, rid, approve_body(r))  # returns 'executing' after 0.2 s
    with pytest.raises(ApiError) as e:
        await lc.start(user, "real", start_body("next"))
    assert e.value.code == "busy"
    await lc.drain()
    await lc.start(user, "real", start_body("next"))  # finished execution blocks nothing


async def test_database_enforces_one_open_request(lc, user, pool):
    await lc.start(user, "real", start_body())
    with pytest.raises(Exception):
        async with pool.connection() as conn:
            await conn.execute(
                "INSERT INTO requests (id, user_id, scope, state, current_revision, timezone, language,"
                " orientation, created_at, updated_at) VALUES (gen_random_uuid(), %s, 'real', 'review',"
                " 1, 'America/Chicago', 'en', 'landscape', now(), now())", (user,))


# ---------- decisions ----------

async def test_select_candidate_then_review(lc, user):
    rid, r = await new_review(lc, user, "email ambiguous saying hi")
    assert r["state"] == "conflict_ambiguous" and len(r["candidates"]) == 2
    with pytest.raises(ApiError) as e:
        await lc.select(user, rid, {"revision": 1, "candidate_id": "nobody_here"})
    assert e.value.code == "invalid_request"
    rev2 = await lc.select(user, rid, {"revision": 1, "candidate_id": "victor_t"})
    r2 = await lc.process(rid, rev2)
    assert r2["state"] == "review" and r2["draft"]["to"] == "victor_t@example.com"


async def test_no_match_gives_empty_candidates(lc, user):
    _, r = await new_review(lc, user, "email nobody saying hi")
    assert r["state"] == "conflict_ambiguous" and r["candidates"] == []


async def test_continue_through_schedule_conflict(lc, user):
    rid, r = await new_review(lc, user, "gym tomorrow at 3 conflict")
    assert r["state"] == "conflict_schedule"
    r2 = await lc.continue_(user, rid, {"revision": 1})
    assert r2["state"] == "review" and r2["screen_lines"] == ["Gym", "Tue 3:00 to 4:00 PM"]
    assert (await lc.continue_(user, rid, {"revision": 1}))["state"] == "review"  # repeat is harmless
    assert (await lc.approve(user, rid, approve_body(r2)))["state"] == "success"


async def test_read_only_result_never_executes(lc, user, executors):
    rid, r = await new_review(lc, user, "what do I have today?")
    assert r["state"] == "read_only_result" and r["proposal_hash"] is None
    with pytest.raises(ApiError):
        await lc.approve(user, rid, {"revision": 1, "proposal_hash": "x"})
    assert executors["mock"].calls == []


async def test_processing_error_is_failure(lc, user):
    _, r = await new_review(lc, user, "explode please")
    assert r["state"] == "failure"


# ---------- scope, screens, recovery ----------

async def test_demo_key_never_reaches_real_tools(lc, user, executors, settings):
    lc.settings = settings.model_copy(update={"real_tools_enabled": True})
    rid, r = await new_review(lc, user, scope="mock")
    await lc.approve(user, rid, approve_body(r))
    assert executors["real"].calls == [] and len(executors["mock"].calls) == 1


async def test_screen_lines_fit_budget(lc, user, settings):
    _, r = await new_review(lc, user, "send a very long note " * 10)
    assert fits(r["screen_lines"], settings, "landscape")


async def test_recover_on_startup(lc, user, executors, pool):
    executors["mock"].hang = True
    rid, r = await new_review(lc, user)
    await lc.approve(user, rid, approve_body(r))  # still executing
    for task in list(lc._tasks):
        task.cancel()  # simulate the process dying mid-execution
    await asyncio.gather(*list(lc._tasks), return_exceptions=True)
    await lc.recover_on_startup()
    assert (await lc.get_result(user, rid))["state"] == "execution_uncertain"


# ---------- gate coverage: every decision call and every decision state ----------

async def test_select_and_continue_reject_stale_revision(lc, user):
    rid, _ = await new_review(lc, user, "email ambiguous saying hi")
    await lc.process(rid, await lc.add(user, rid, addition_body("ambiguous still", 1)))
    with pytest.raises(ApiError) as e:
        await lc.select(user, rid, {"revision": 1, "candidate_id": "victor_t"})
    assert e.value.code == "stale_revision"
    rid2, _ = await new_review(lc, user, "gym at 3 conflict")
    await lc.process(rid2, await lc.add(user, rid2, addition_body("conflict again", 1)))
    with pytest.raises(ApiError) as e:
        await lc.continue_(user, rid2, {"revision": 1})
    assert e.value.code == "stale_revision"


@pytest.mark.parametrize("text,call", [
    ("email ambiguous saying hi", lambda lc, u, rid: lc.select(u, rid, {"revision": 1, "candidate_id": "victor_t"})),
    ("gym at 3 conflict", lambda lc, u, rid: lc.continue_(u, rid, {"revision": 1})),
])
async def test_conflict_states_expire_after_60_s(lc, user, clock, text, call):
    rid, r = await new_review(lc, user, text)
    assert r["expires_in_s"] == 60
    clock.advance(61)
    with pytest.raises(ApiError) as e:
        await call(lc, user, rid)
    assert e.value.code == "expired" and e.value.current["state"] == "cancelled"
