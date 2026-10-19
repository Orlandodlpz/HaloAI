"""Demo site endpoints (B6): visitor sessions, isolation, caps and the panels' read endpoints."""

import uuid

import pytest

from app.mockstore import MockStore
from tests.conftest import auth, start_body


def cid():
    return {"call_id": str(uuid.uuid4())}


@pytest.fixture
def demo_services(services, pool, clock):
    services.store = MockStore(pool, clock)
    return services


async def session(client, key):
    r = await client.post("/v1/demo/session", headers=auth(key), json=cid())
    return r, r.json().get("token")


async def visitor_user(pool, token):
    from app import ids
    async with pool.connection() as conn:
        cur = await conn.execute("SELECT u.id, u.sandbox, u.expires_at, k.scope, k.client FROM api_keys k"
                                 " JOIN users u ON u.id = k.user_id WHERE k.key_hash = %s", (ids.key_hash(token),))
        return await cur.fetchone()


# ---------- sessions ----------

async def test_each_visitor_gets_their_own_sandbox_user(client, keys, pool, demo_services):
    r, a = await session(client, keys["demo"])
    _, b = await session(client, keys["demo"])
    assert r.status_code == 200 and a and b and a != b and r.json()["expires_at"]
    ua, ub = await visitor_user(pool, a), await visitor_user(pool, b)
    assert ua["id"] != ub["id"] and ua["sandbox"] and ub["sandbox"] and ua["expires_at"] is not None
    assert ua["scope"] == "mock" and ua["client"] == "visitor"
    assert (await client.get("/health", headers=auth(a))).status_code == 200


async def test_only_the_demo_site_key_starts_sessions(client, keys, demo_services):
    _, visitor = await session(client, keys["demo"])
    again, _ = await session(client, visitor)
    phone, _ = await session(client, keys["phone"])
    assert again.status_code == phone.status_code == 422


async def test_expired_visitor_key_is_refused(client, keys, pool, demo_services):
    """Also by time: a session older than demo_session_hours stops working."""
    _, token = await session(client, keys["demo"])
    from app import ids
    async with pool.connection() as conn:
        await conn.execute("UPDATE api_keys SET expires_at = %s WHERE key_hash = %s",
                           (demo_services.lc.clock.now(), ids.key_hash(token)))
    assert (await client.get("/health", headers=auth(token))).status_code == 401


async def test_new_sessions_are_limited_per_address(client, keys, demo_services, settings):
    demo_services.settings = settings.model_copy(update={"demo_sessions_per_ip_hour": 2})
    codes = [(await session(client, keys["demo"]))[0] for _ in range(3)]
    assert [c.status_code for c in codes] == [200, 200, 429] and codes[2].headers["retry-after"] == "600"


# ---------- the panels ----------

async def test_calendar_is_seeded_per_visitor_and_isolated(client, keys, demo_services, pool, clock):
    _, a = await session(client, keys["demo"])
    _, b = await session(client, keys["demo"])
    ca = (await client.get("/v1/demo/calendar", headers=auth(a))).json()["events"]
    assert {"Algorithms lecture", "Office hours with Professor Patel", "Gym with Marcus"} <= {e["title"] for e in ca}  # a student's week
    ua = str((await visitor_user(pool, a))["id"])
    await demo_services.store.apply(ua, "k" * 26, {"kind": "delete_event", "event_id": "e3",
                                                   "event": {"title": "Office hours with Professor Patel"}})
    ca2 = (await client.get("/v1/demo/calendar", headers=auth(a))).json()["events"]
    cb = (await client.get("/v1/demo/calendar", headers=auth(b))).json()["events"]
    office = "Office hours with Professor Patel"
    assert office not in {e["title"] for e in ca2} and office in {e["title"] for e in cb}


async def test_outbox_shows_only_the_visitors_own_emails(client, keys, demo_services, pool):
    _, a = await session(client, keys["demo"])
    _, b = await session(client, keys["demo"])
    ua = str((await visitor_user(pool, a))["id"])
    await demo_services.store.apply(ua, "m" * 26, {"kind": "send_email", "to": "victor.morales@example.com",
                                                   "subject": "Demo", "body": "Ready."})
    ea = (await client.get("/v1/demo/outbox", headers=auth(a))).json()["emails"]
    eb = (await client.get("/v1/demo/outbox", headers=auth(b))).json()["emails"]
    assert [e["to"] for e in ea] == ["victor.morales@example.com"] and eb == []


async def test_contacts_are_the_sample_address_book(client, keys, demo_services):
    _, a = await session(client, keys["demo"])
    people = (await client.get("/v1/demo/contacts", headers=auth(a))).json()["contacts"]
    assert {"name": "Victor Morales", "email": "victor.morales@example.com"} in people
    assert {"name": "TA Jordan Lee", "email": "jordan.lee@example.com"} in people  # "email the TA" resolves
    assert all(p["email"].endswith("example.com") for p in people)


async def test_panels_are_demo_only_and_check_the_timezone(client, keys, demo_services):
    for path in ("/v1/demo/calendar", "/v1/demo/outbox", "/v1/demo/contacts"):
        assert (await client.get(path, headers=auth(keys["phone"]))).status_code == 422
        assert (await client.get(path)).status_code == 401
    _, a = await session(client, keys["demo"])
    assert (await client.get("/v1/demo/calendar", headers=auth(a), params={"tz": "Mars/Base"})).status_code == 422


# ---------- the shared daily budget ----------

async def start(client, key):
    return await client.post("/v1/requests", headers=auth(key), json={**start_body(), "call_id": str(uuid.uuid4())})


async def test_daily_budget_covers_all_sample_mode_keys(client, keys, demo_services, settings):
    demo_services.settings = settings.model_copy(update={"demo_daily_requests": 2})
    _, a = await session(client, keys["demo"])
    _, b = await session(client, keys["demo"])
    assert (await start(client, a)).status_code == 200
    assert (await start(client, b)).status_code == 200
    busy = await start(client, a)
    assert busy.status_code == 429 and "busy today" in busy.json()["error"]["message"]
    assert (await start(client, keys["demo"])).status_code == 429  # the public demo key is capped too
    assert (await start(client, keys["phone"])).status_code == 200  # the phone is never capped


async def test_budget_resets_after_24_hours(client, keys, demo_services, settings, clock):
    demo_services.settings = settings.model_copy(update={"demo_daily_requests": 1})
    _, a = await session(client, keys["demo"])
    assert (await start(client, a)).status_code == 200
    assert (await start(client, a)).status_code == 429
    clock.advance(24 * 3600 + 1)
    _, fresh = await session(client, keys["demo"])  # the old session expired with the day as well
    assert (await start(client, fresh)).status_code == 200


async def test_session_expires_after_its_hours(client, keys, demo_services, clock):
    _, token = await session(client, keys["demo"])
    assert (await client.get("/health", headers=auth(token))).status_code == 200
    clock.advance(24 * 3600 + 1)
    assert (await client.get("/health", headers=auth(token))).status_code == 401


async def test_additions_and_selections_are_capped_too(client, keys, demo_services, settings):
    _, a = await session(client, keys["demo"])
    body = start_body()
    assert (await client.post("/v1/requests", headers=auth(a), json={**body, "call_id": str(uuid.uuid4())})).status_code == 200
    demo_services.settings = settings.model_copy(update={"demo_daily_requests": 1})  # now used up
    rid = body["request_id"]
    add = await client.post(f"/v1/requests/{rid}/additions", headers=auth(a), json={
        "call_id": str(uuid.uuid4()), "segment_id": str(uuid.uuid4()), "text": "make it 4", "revision": 1,
        "sent_at": "2026-10-12T15:00:30-05:00"})
    sel = await client.post(f"/v1/requests/{rid}/select", headers=auth(a), json={
        "call_id": str(uuid.uuid4()), "segment_id": str(uuid.uuid4()), "candidate_id": "c_victor", "revision": 1})
    assert add.status_code == 429 and sel.status_code == 429
