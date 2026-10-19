"""Memory endpoints (B3 step 3) and the separate demo user."""

import uuid

import pytest

from app.auth import create_key
from tests.conftest import auth, start_body


def cid():
    return str(uuid.uuid4())


async def add(client, key, kind="contact", label="Victor", value="victor at example dot com"):
    return await client.post("/v1/memory", headers=auth(key), json={"call_id": cid(), "kind": kind, "label": label, "value": value})


async def test_add_list_edit_delete(client, keys):
    assert (await client.get("/v1/memory", headers=auth(keys["phone"]))).json() == {"items": []}
    r = await add(client, keys["phone"])
    item = r.json()
    assert r.status_code == 200 and item["value"] == "victor@example.com" and item["source"] == "user"
    listed = (await client.get("/v1/memory", headers=auth(keys["phone"]))).json()["items"]
    assert [i["memory_id"] for i in listed] == [item["memory_id"]]
    r = await client.patch(f"/v1/memory/{item['memory_id']}", headers=auth(keys["phone"]),
                           json={"call_id": cid(), "value": "victor.new@example.com"})
    assert r.status_code == 200 and r.json()["value"] == "victor.new@example.com"
    call = cid()
    r1 = await client.request("DELETE", f"/v1/memory/{item['memory_id']}", headers=auth(keys["phone"]), json={"call_id": call})
    r2 = await client.request("DELETE", f"/v1/memory/{item['memory_id']}", headers=auth(keys["phone"]), json={"call_id": call})
    assert r1.json() == r2.json() == {"deleted": True}  # a retried delete replays, never errors
    r3 = await client.request("DELETE", f"/v1/memory/{item['memory_id']}", headers=auth(keys["phone"]), json={"call_id": cid()})
    assert r3.status_code == 404 and r3.json()["error"]["code"] == "not_found"


@pytest.mark.parametrize("payload,code", [
    ({"kind": "contact", "label": "Sam", "value": "not an address"}, "invalid_request"),
    ({"kind": "secret", "label": "x", "value": "y"}, "invalid_request"),
    ({"kind": "fact", "label": "", "value": "y"}, "invalid_request"),
])
async def test_add_validation(client, keys, payload, code):
    r = await client.post("/v1/memory", headers=auth(keys["phone"]), json={"call_id": cid(), **payload})
    assert r.status_code == 422 and r.json()["error"]["code"] == code


async def test_edit_errors(client, keys):
    item = (await add(client, keys["phone"])).json()
    nothing = await client.patch(f"/v1/memory/{item['memory_id']}", headers=auth(keys["phone"]), json={"call_id": cid()})
    assert nothing.status_code == 422
    missing = await client.patch(f"/v1/memory/{uuid.uuid4()}", headers=auth(keys["phone"]), json={"call_id": cid(), "label": "x"})
    assert missing.status_code == 404
    action = await client.patch("/v1/memory/act_abc", headers=auth(keys["phone"]), json={"call_id": cid(), "label": "x"})
    assert action.status_code == 422 and "deleted, not edited" in action.json()["error"]["message"]


async def test_demo_site_cannot_see_or_touch_phone_memory(client, keys):
    item = (await add(client, keys["phone"])).json()
    assert (await client.get("/v1/memory", headers=auth(keys["demo"]))).json() == {"items": []}
    r = await client.request("DELETE", f"/v1/memory/{item['memory_id']}", headers=auth(keys["demo"]), json={"call_id": cid()})
    assert r.status_code == 404
    still = (await client.get("/v1/memory", headers=auth(keys["phone"]))).json()["items"]
    assert [i["memory_id"] for i in still] == [item["memory_id"]]


async def test_key_scope_must_match_the_kind_of_user(pool, user, demo_user):
    with pytest.raises(ValueError):
        await create_key(pool, user, "oops", "mock")  # a demo key on the phone user
    with pytest.raises(ValueError):
        await create_key(pool, demo_user, "oops", "real")  # a phone key on the sandbox


async def test_recent_actions_listed_and_forgettable(client, keys, lc, user, pool):
    body = start_body("block 3 to 4 tomorrow for the gym")
    await lc.start(user, "real", body)
    review = await lc.process(body["request_id"], 1)
    done = await lc.approve(user, body["request_id"], {"revision": 1, "proposal_hash": review["proposal_hash"]})
    items = (await client.get("/v1/memory", headers=auth(keys["phone"]))).json()["items"]
    action = next(i for i in items if i["kind"] == "action")
    assert action["memory_id"] == "act_" + done["execution_key"] and action["source"] == "action"
    r = await client.request("DELETE", f"/v1/memory/{action['memory_id']}", headers=auth(keys["phone"]), json={"call_id": cid()})
    assert r.json() == {"deleted": True}
    assert not any(i["kind"] == "action" for i in (await client.get("/v1/memory", headers=auth(keys["phone"]))).json()["items"])
    async with pool.connection() as conn:  # the execution record stays: approval exactly once needs it
        cur = await conn.execute("SELECT status, forgotten FROM executions WHERE execution_key = %s", (done["execution_key"],))
        assert dict(await cur.fetchone()) == {"status": "applied", "forgotten": True}


async def test_edited_or_deleted_memory_changes_the_next_answer(client, keys, lc, user, pool, clock):
    """Gate item 2, end to end: teach Victor, edit him, delete him; each changes where 'email Victor' goes."""
    from app.agent import NemotronProcessor
    from app.config import get_settings
    from app.memory import MemoryStore
    from app.mockstore import MockStore
    from tests.test_agent import ScriptedClient, reply, tool_call

    store, mem = MockStore(pool, clock), MemoryStore(pool, clock)

    async def user_world(ctx, now, tz):
        world = await store.world(ctx.user_id, now, tz)
        snap = await mem.load(ctx.user_id)
        world.memory_items, world.actions = snap.items, snap.actions
        return world

    script = ScriptedClient([reply([tool_call("draft_email", {"recipient": "Victor", "subject": "Hi", "body": "Hi"})])
                             for _ in range(3)])
    lc.processor = NemotronProcessor(script, get_settings(), world_factory=user_world)

    async def email_victor():
        body = start_body("email victor saying hi")
        await lc.start(user, "real", body)
        return (await lc.process(body["request_id"], 1))["draft"]["to"]

    item = (await add(client, keys["phone"])).json()
    assert await email_victor() == "victor@example.com"
    await client.patch(f"/v1/memory/{item['memory_id']}", headers=auth(keys["phone"]),
                       json={"call_id": cid(), "value": "victor.new@example.com"})
    assert await email_victor() == "victor.new@example.com"
    await client.request("DELETE", f"/v1/memory/{item['memory_id']}", headers=auth(keys["phone"]), json={"call_id": cid()})
    assert await email_victor() == "victor.morales@example.com"  # back to the address book
