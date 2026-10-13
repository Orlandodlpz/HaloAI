"""JSON endpoints over HTTP: keys and scopes, rate limits, call_id retries, error shape."""

import asyncio
import uuid

from tests.conftest import addition_body, auth, start_body


async def review(lc, user, text="block 3 to 4 tomorrow for the gym", scope="real"):
    body = start_body(text)
    await lc.start(user, scope, body)
    return body["request_id"], await lc.process(body["request_id"], 1)


def approve_json(result, call_id=None):
    return {"call_id": call_id or str(uuid.uuid4()), "revision": result["revision"],
            "proposal_hash": result["proposal_hash"]}


# ---------- keys ----------

async def test_health_requires_key(client, keys):
    assert (await client.get("/health")).status_code == 401
    assert (await client.get("/health", headers=auth("wrong"))).status_code == 401
    r = await client.get("/health", headers=auth(keys["phone"]))
    assert r.status_code == 200 and r.json() == {"status": "ok"}


async def test_error_shape_for_missing_key(client):
    r = await client.post(f"/v1/requests/{uuid.uuid4()}/cancel", json={"call_id": str(uuid.uuid4())})
    assert r.status_code == 401
    assert r.json() == {"error": {"code": "unauthorized", "message": "missing API key", "current": None}}


async def test_revoked_key_rejected(client, keys, pool):
    async with pool.connection() as conn:
        await conn.execute("UPDATE api_keys SET revoked_at = now() WHERE client = 'phone'")
    assert (await client.get("/health", headers=auth(keys["phone"]))).status_code == 401


async def test_rate_limit_per_key(client, keys):
    codes = [(await client.get("/health", headers=auth(keys["slow"]))).status_code for _ in range(4)]
    assert codes == [200, 200, 200, 429]
    r = await client.get("/health", headers=auth(keys["slow"]))
    assert r.json()["error"]["code"] == "rate_limited"
    assert 1 <= int(r.headers["retry-after"]) <= 20  # 3 per minute: one every 20 s
    assert (await client.get("/health", headers=auth(keys["phone"]))).status_code == 200  # other keys unaffected


async def test_invalid_body_uses_error_shape(client, keys):
    r = await client.post(f"/v1/requests/{uuid.uuid4()}/approve", headers=auth(keys["phone"]),
                          json={"call_id": "not-a-uuid", "revision": 0})
    assert r.status_code == 422 and r.json()["error"]["code"] == "invalid_request"


# ---------- approve, retries ----------

async def test_approve_happy_path_with_timings(client, keys, lc, user):
    rid, res = await review(lc, user)
    r = await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json=approve_json(res))
    body = r.json()
    assert r.status_code == 200 and body["state"] == "success"
    assert body["timings"]["backend_total_ms"] >= 0 and len(body["execution_key"]) == 26


async def test_repeated_call_id_replays_original_answer(client, keys, lc, user, executors):
    rid, res = await review(lc, user)
    payload = approve_json(res)
    first = await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json=payload)
    second = await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json=payload)
    assert first.json() == second.json()  # byte for byte the stored answer, timings included
    assert len(executors["mock"].calls) == 1


async def test_concurrent_same_call_id_runs_once(client, keys, lc, user, executors):
    rid, res = await review(lc, user)
    payload = approve_json(res)
    rs = await asyncio.gather(*[client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]),
                                            json=payload) for _ in range(4)])
    assert {r.status_code for r in rs} == {200}
    assert len({r.text for r in rs}) == 1 and len(executors["mock"].calls) == 1


async def test_same_call_id_different_body_conflicts(client, keys, lc, user):
    rid, res = await review(lc, user)
    payload = approve_json(res)
    await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json=payload)
    changed = {**payload, "revision": payload["revision"] + 1}
    r = await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json=changed)
    assert r.status_code == 409 and r.json()["error"]["code"] == "call_id_conflict"


async def test_call_id_kept_24_hours(client, keys, lc, user, clock):
    rid, res = await review(lc, user)
    call_id = str(uuid.uuid4())
    await client.post(f"/v1/requests/{rid}/cancel", headers=auth(keys["phone"]), json={"call_id": call_id})
    clock.advance(25 * 3600)
    # After retention, the same call_id is a new call (here: a different path, no conflict).
    r = await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]),
                          json=approve_json(res, call_id))
    assert r.json()["error"]["code"] != "call_id_conflict"


async def test_stale_revision_includes_current(client, keys, lc, user):
    rid, res = await review(lc, user)
    await lc.process(rid, await lc.add(user, rid, addition_body("make it 4", 1)))
    r = await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json=approve_json(res))
    err = r.json()["error"]
    assert r.status_code == 409 and err["code"] == "stale_revision" and err["current"]["revision"] == 2


async def test_wrong_hash_is_409(client, keys, lc, user):
    rid, res = await review(lc, user)
    bad = {**approve_json(res), "proposal_hash": "0" * 64}
    r = await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json=bad)
    assert r.status_code == 409 and r.json()["error"]["code"] == "hash_mismatch"


async def test_slow_approve_then_poll_status(client, keys, lc, user, executors):
    executors["mock"].delay_s = 0.35
    rid, res = await review(lc, user)
    r = await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json=approve_json(res))
    assert r.json()["state"] == "executing"
    key = r.json()["execution_key"]
    await lc.drain()
    s = await client.get(f"/v1/executions/{key}", headers=auth(keys["phone"]))
    assert s.status_code == 200 and s.json()["state"] == "success"
    missing = await client.get("/v1/executions/doesnotexist", headers=auth(keys["phone"]))
    assert missing.status_code == 404 and missing.json()["error"]["code"] == "not_found"


# ---------- cancel, continue, touch ----------

async def test_cancel_then_after_approve(client, keys, lc, user):
    rid, res = await review(lc, user)
    await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json=approve_json(res))
    r = await client.post(f"/v1/requests/{rid}/cancel", headers=auth(keys["phone"]),
                          json={"call_id": str(uuid.uuid4())})
    assert r.status_code == 409 and r.json()["error"]["code"] == "already_executing"
    assert r.json()["error"]["current"]["state"] == "success"


async def test_continue_and_touch(client, keys, lc, user, clock):
    rid, res = await review(lc, user, "gym at 3 conflict")
    r = await client.post(f"/v1/requests/{rid}/continue", headers=auth(keys["phone"]),
                          json={"call_id": str(uuid.uuid4()), "revision": 1})
    assert r.json()["state"] == "review"
    clock.advance(55)
    t = await client.post(f"/v1/requests/{rid}/touch", headers=auth(keys["phone"]), json={"revision": 1})
    assert t.status_code == 200 and t.json()["expires_in_s"] == 60


async def test_expired_request_returns_409_expired(client, keys, lc, user, clock):
    rid, res = await review(lc, user)
    clock.advance(61)
    r = await client.post(f"/v1/requests/{rid}/approve", headers=auth(keys["phone"]), json=approve_json(res))
    assert r.status_code == 409 and r.json()["error"]["code"] == "expired"
    assert r.json()["error"]["current"]["state"] == "cancelled"


async def test_unknown_request_404(client, keys):
    r = await client.post(f"/v1/requests/{uuid.uuid4()}/cancel", headers=auth(keys["phone"]),
                          json={"call_id": str(uuid.uuid4())})
    assert r.status_code == 404 and r.json()["error"]["code"] == "not_found"
