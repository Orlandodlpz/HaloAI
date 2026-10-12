"""Streaming endpoints over HTTP: event order, revisions, slow, ping, retries."""

import asyncio
import json
import uuid

from tests.conftest import auth, start_body


def parse_sse(text: str) -> tuple[list[tuple[str, dict]], list[str]]:
    """Returns (events, comments) the way an SSE client sees them."""
    events, comments = [], []
    for block in text.replace("\r\n", "\n").split("\n\n"):
        name, data = "message", None
        for line in block.split("\n"):
            if line.startswith(":"):
                comments.append(line[1:].strip())
            elif line.startswith("event:"):
                name = line[6:].strip()
            elif line.startswith("data:"):
                data = json.loads(line[5:].strip())
        if data is not None:
            events.append((name, data))
    return events, comments


def start_json(text="block 3 to 4 tomorrow for the gym", **extra):
    body = start_body(text, **extra)
    body["call_id"] = str(uuid.uuid4())
    return body


async def post_stream(client, key, path, payload):
    r = await client.post(path, headers=auth(key), json=payload)
    return r, parse_sse(r.text)[0] if r.headers["content-type"].startswith("text/event-stream") else None


async def test_start_event_order_and_revision_on_every_event(client, keys):
    r, events = await post_stream(client, keys["phone"], "/v1/requests", start_json())
    assert r.status_code == 200 and r.headers["content-type"].startswith("text/event-stream")
    names = [e for e, _ in events]
    assert names[0] == "accepted" and names[-1] == "result" and names.count("result") == 1
    assert "status" in names
    rid = events[0][1]["request_id"]
    assert all(d["request_id"] == rid and d["revision"] == 1 for _, d in events)
    result = events[-1][1]
    assert result["state"] == "review" and result["timings"]["backend_total_ms"] >= 0


async def test_addition_and_select_streams(client, keys):
    _, events = await post_stream(client, keys["phone"], "/v1/requests", start_json("email ambiguous saying hi"))
    rid = events[0][1]["request_id"]
    assert events[-1][1]["state"] == "conflict_ambiguous"
    _, sel = await post_stream(client, keys["phone"], f"/v1/requests/{rid}/select",
                               {"call_id": str(uuid.uuid4()), "revision": 1, "candidate_id": "victor_r"})
    assert sel[0] == ("accepted", {"request_id": rid, "revision": 2, "call_id": sel[0][1]["call_id"]})
    assert sel[-1][1]["state"] == "review" and sel[-1][1]["revision"] == 2
    _, add = await post_stream(client, keys["phone"], f"/v1/requests/{rid}/additions", {
        "call_id": str(uuid.uuid4()), "revision": 1,  # stale base on purpose: still appended
        "segment_id": str(uuid.uuid4()), "text": "and say thanks", "sent_at": "2026-10-12T15:01:00-05:00"})
    assert add[0][1]["revision"] == 3 and add[-1][1]["revision"] == 3


async def test_finished_stream_replays_without_rerunning(client, keys, processor):
    payload = start_json()
    _, first = await post_stream(client, keys["phone"], "/v1/requests", payload)
    _, again = await post_stream(client, keys["phone"], "/v1/requests", payload)
    assert [e for e, _ in again] == ["accepted", "result"]
    assert again[0][1] == first[0][1] and again[-1][1] == first[-1][1]
    assert len(processor.calls) == 1


async def test_retry_reattaches_to_running_stream(client, keys, processor):
    processor.gate = asyncio.Event()
    payload = start_json()
    first = asyncio.create_task(post_stream(client, keys["phone"], "/v1/requests", payload))
    await asyncio.sleep(0.1)  # first call is now waiting inside the processor
    second = asyncio.create_task(post_stream(client, keys["phone"], "/v1/requests", payload))
    await asyncio.sleep(0.1)
    processor.gate.set()
    (_, e1), (_, e2) = await first, await second
    assert [e for e, _ in e2] == ["accepted", "status", "result"]
    assert e1[-1][1] == e2[-1][1] and len(processor.calls) == 1


async def test_slow_event_before_result(client, keys, processor, settings):
    settings.slow_threshold_s = 0.1
    processor.delay_s = 0.3
    _, events = await post_stream(client, keys["phone"], "/v1/requests", start_json())
    names = [e for e, _ in events]
    assert names.index("slow") < names.index("result")
    slow = dict(events)["slow"]
    assert slow["revision"] == 1 and slow["request_id"] == events[0][1]["request_id"]


async def test_ping_comments_keep_connection_alive(client, keys, processor, settings):
    settings.sse_ping_s = 0.1
    processor.delay_s = 0.35
    r = await client.post("/v1/requests", headers=auth(keys["phone"]), json=start_json())
    _, comments = parse_sse(r.text)
    assert comments.count("ping") >= 2


async def test_refused_start_is_plain_json_and_replays(client, keys, lc, user, executors):
    executors["mock"].delay_s = 0.5
    _, events = await post_stream(client, keys["phone"], "/v1/requests", start_json())
    res = events[-1][1]
    rid = res["request_id"]
    await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json={
        "call_id": str(uuid.uuid4()), "revision": 1, "proposal_hash": res["proposal_hash"]})  # still executing
    payload = start_json("next command")
    r1 = await client.post("/v1/requests", headers=auth(keys["phone"]), json=payload)
    await lc.drain()  # the old execution finishes: a NEW call would now be accepted
    r2 = await client.post("/v1/requests", headers=auth(keys["phone"]), json=payload)
    # ...but a retry of the refused call returns its original answer, per the contract.
    assert r1.status_code == r2.status_code == 409
    assert r1.json()["error"]["code"] == "busy" and r1.json() == r2.json()
    fresh = await client.post("/v1/requests", headers=auth(keys["phone"]), json=start_json("next command"))
    assert fresh.status_code == 200


async def test_abandoned_call_answers_current_state_without_redoing(client, keys, processor, pool):
    payload = start_json()
    await post_stream(client, keys["phone"], "/v1/requests", payload)
    async with pool.connection() as conn:  # as if the server died before storing the answer
        await conn.execute("UPDATE call_log SET status = 'abandoned', response = NULL WHERE call_id = %s",
                           (payload["call_id"],))
    _, events = await post_stream(client, keys["phone"], "/v1/requests", payload)
    assert [e for e, _ in events] == ["accepted", "result"] and events[-1][1]["state"] == "review"
    assert len(processor.calls) == 1


async def test_demo_key_requests_are_mock_scoped(client, keys, pool):
    _, events = await post_stream(client, keys["demo"], "/v1/requests", start_json())
    async with pool.connection() as conn:
        cur = await conn.execute("SELECT scope FROM requests WHERE id = %s", (events[0][1]["request_id"],))
        assert (await cur.fetchone())["scope"] == "mock"


async def test_start_validation(client, keys):
    for bad in ({"timezone": "CST"}, {"sent_at": "2026-10-12T15:00:00"}, {"text": ""},
                {"orientation": "sideways"}):
        r = await client.post("/v1/requests", headers=auth(keys["phone"]), json={**start_json(), **bad})
        assert r.status_code == 422 and r.json()["error"]["code"] == "invalid_request", bad
