"""Mock world per user (B3 step 1): seeded once, changed only by approved actions."""

from datetime import datetime
from zoneinfo import ZoneInfo

import pytest

from app.mockstore import MockExecutor, MockStore, created_event_id

TZ = ZoneInfo("America/Chicago")
NOW = datetime(2026, 10, 13, 15, 0, tzinfo=TZ)


@pytest.fixture
async def store(pool, clock):
    return MockStore(pool, clock)


def create_draft(title="Gym", start="2026-10-14T15:00:00-05:00", end="2026-10-14T16:00:00-05:00"):
    return {"kind": "create_event", "title": title, "start": start, "end": end, "timezone": "America/Chicago",
            "guests": [], "video_call": False, "description": ""}


async def test_seeded_once_and_stable_over_days(store, user):
    first = await store.world(user, NOW, TZ)
    assert [e["event_id"] for e in first.events][:3] == ["e1", "e2", "e3"]
    later = await store.world(user, datetime(2026, 10, 16, 9, 0, tzinfo=TZ), TZ)
    assert [(e["event_id"], e["start"]) for e in later.events] == [(e["event_id"], e["start"]) for e in first.events]


async def test_users_do_not_share_a_calendar(store, user, lc):
    other = await lc.ensure_user("someone-else")
    await store.apply(user, "k" * 26, create_draft())
    mine = await store.world(user, NOW, TZ)
    theirs = await store.world(other, NOW, TZ)
    assert any(e["title"] == "Gym" for e in mine.events)
    assert not any(e["title"] == "Gym" for e in theirs.events)


async def test_create_is_visible_and_retry_safe(store, user):
    await store.world(user, NOW, TZ)
    key = "abcdefgh" + "x" * 18
    out1 = await store.apply(user, key, create_draft())
    out2 = await store.apply(user, key, create_draft())  # retried approval: same key
    assert out1.applied and out2.applied and out1.screen_lines[0] == "Added to your calendar"
    world = await store.world(user, NOW, TZ)
    assert [e["event_id"] for e in world.events].count(created_event_id(key)) == 1


async def test_move_and_delete(store, user):
    await store.world(user, NOW, TZ)
    move = {"kind": "update_event", "event_id": "e3", "timezone": "America/Chicago",
            "after": {"title": "Dentist", "start": "2026-10-14T14:00:00-05:00", "end": "2026-10-14T15:00:00-05:00"}}
    assert (await store.apply(user, "m" * 26, move)).applied
    world = await store.world(user, NOW, TZ)
    assert next(e for e in world.events if e["event_id"] == "e3")["start"].hour == 14
    delete = {"kind": "delete_event", "event_id": "e3", "event": {"title": "Dentist"}}
    assert (await store.apply(user, "d" * 26, delete)).applied
    assert (await store.apply(user, "d" * 26, delete)).applied  # already gone still counts as done
    world = await store.world(user, NOW, TZ)
    assert all(e["event_id"] != "e3" for e in world.events)
    gone = await store.apply(user, "u" * 26, move)  # moving a deleted event
    assert not gone.applied


async def test_email_goes_to_outbox_once(store, user, pool):
    draft = {"kind": "send_email", "to": "victor.morales@example.com", "subject": "Hi", "body": "Hello"}
    await store.apply(user, "e" * 26, draft)
    await store.apply(user, "e" * 26, draft)
    async with pool.connection() as conn:
        cur = await conn.execute("SELECT count(*) AS n FROM mock_outbox WHERE user_id = %s", (user,))
        assert (await cur.fetchone())["n"] == 1


async def test_executor_requires_a_user(store):
    with pytest.raises(ValueError):
        await MockExecutor(store).execute(create_draft(), "k" * 26)


async def test_approved_event_appears_in_the_next_request(lc, user, pool, clock):
    """End to end: approve 'block the gym', and the next request's prompt sees it."""
    from app.agent import NemotronProcessor
    from app.config import get_settings
    from tests.conftest import start_body
    from tests.test_agent import ScriptedClient, reply, tool_call

    store = MockStore(pool, clock)

    async def user_world(ctx, now, tz):
        return await store.world(ctx.user_id, now, tz)

    client = ScriptedClient([
        reply([tool_call("create_event", {"title": "Gym", "start": "2026-10-14T15:00", "end": "2026-10-14T16:00"})]),
        reply(content="You have the gym at 3 PM tomorrow."),
    ])
    lc.processor = NemotronProcessor(client, get_settings(), world_factory=user_world)
    lc.executors = {"mock": MockExecutor(store), "real": MockExecutor(store)}

    body = start_body("block 3 to 4 tomorrow for the gym", sent_at="2026-10-13T15:00:00-05:00")
    await lc.start(user, "real", body)
    review = await lc.process(body["request_id"], 1)
    done = await lc.approve(user, body["request_id"], {"revision": 1, "proposal_hash": review["proposal_hash"]})
    assert done["state"] == "success" and done["screen_lines"][0] == "Added to your calendar"

    second = start_body("what do I have tomorrow", sent_at="2026-10-13T15:05:00-05:00")
    await lc.start(user, "real", second)
    await lc.process(second["request_id"], 1)
    system_prompt = client.requests[1]["messages"][0]["content"]
    assert "Gym" in system_prompt and created_event_id(done["execution_key"]) in system_prompt
