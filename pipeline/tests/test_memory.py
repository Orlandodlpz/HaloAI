"""Memory (B3 step 2): learned only from approved actions, "remember that" only in the user's own
words, memory before the address book, and only relevant memory in the prompt."""

from datetime import datetime
from zoneinfo import ZoneInfo

import pytest

from app.agent import NemotronProcessor
from app.config import get_settings
from app.memory import MemoryStore, remember_intent
from app.mockdata import build_world
from app.mockstore import MockExecutor, MockStore
from app.processing import Segment
from app.prompt import memory_context
from app.tools import resolve_contacts
from tests.conftest import start_body
from tests.test_agent import ScriptedClient, reply, tool_call

TZ = ZoneInfo("America/Chicago")
NOW = datetime(2026, 10, 13, 15, 0, tzinfo=TZ)


@pytest.fixture
async def mem(pool, clock):
    return MemoryStore(pool, clock)


@pytest.fixture
async def wired(lc, pool, clock, mem):
    """The real lifecycle with memory, the user's mock world and a scripted model."""
    store = MockStore(pool, clock)

    async def user_world(ctx, now, tz):
        world = await store.world(ctx.user_id, now, tz)
        snap = await mem.load(ctx.user_id)
        world.memory_items, world.actions = snap.items, snap.actions
        return world

    lc.memory = mem
    lc.executors = {"mock": MockExecutor(store), "real": MockExecutor(store)}

    def script(*replies):
        client = ScriptedClient(list(replies))
        lc.processor = NemotronProcessor(client, get_settings(), world_factory=user_world)
        return client
    return script


async def run(lc, user, text, minute=0, approve=True):
    body = start_body(text, sent_at=f"2026-10-13T15:{minute:02d}:00-05:00")
    await lc.start(user, "real", body)
    result = await lc.process(body["request_id"], 1)
    if approve and result["state"] == "review":
        result = await lc.approve(user, body["request_id"], {"revision": 1, "proposal_hash": result["proposal_hash"]})
    return body["request_id"], result


def email(to, body="Hi", subject="Hi"):
    return reply([tool_call("draft_email", {"recipient": to, "subject": subject, "body": body})])


async def contacts(mem, user):
    return [(m["label"], m["value"], m["source"], m["uses"]) for m in (await mem.load(user)).items if m["kind"] == "contact"]


# ---------- learning from approved actions ----------

async def test_approved_emails_are_learned_with_a_use_count(lc, user, wired, mem):
    wired(email("Victor"), email("Victor"))
    await run(lc, user, "email victor saying hi", 0)
    await run(lc, user, "email victor saying hi again", 1)
    assert await contacts(mem, user) == [("Victor Morales", "victor.morales@example.com", "learned", 2)]


async def test_declined_or_unapproved_actions_teach_nothing(lc, user, wired, mem):
    wired(email("Maria"))
    await run(lc, user, "email maria saying hi", approve=False)
    assert await contacts(mem, user) == []


async def test_invited_guests_are_learned(lc, user, wired, mem):
    wired(reply([tool_call("create_event", {"title": "Call with Maria", "start": "2026-10-16T16:00",
                                            "guests": ["Maria"], "video_call": True})]))
    await run(lc, user, "set up a call with maria friday at 4")
    assert await contacts(mem, user) == [("Maria Lopez", "maria.lopez@example.com", "learned", 1)]


async def test_usual_event_length_is_learned_after_three(lc, user, wired, mem):
    wired(*[reply([tool_call("create_event", {"title": f"Focus {i}", "start": f"2026-10-2{i}T09:00",
                                              "end": f"2026-10-2{i}T09:30"})]) for i in range(3)])
    for i in range(3):
        await run(lc, user, f"focus block {i}", i)
    prefs = [(m["label"], m["value"]) for m in (await mem.load(user)).items if m["kind"] == "preference"]
    assert prefs == [("usual event length", "30 minutes")]


# ---------- "remember that" ----------

async def test_remember_that_saves_and_then_wins_over_the_address_book(lc, user, wired, mem):
    client = wired(
        reply([tool_call("remember", {"kind": "contact", "label": "Victor", "value": "victor at utrgv dot edu"})]),
        email("Victor"),
    )
    _, saved = await run(lc, user, "remember that victor's email is victor at utrgv dot edu", 0)
    assert saved["state"] == "read_only_result" and saved["screen_lines"] == ["Saved", "Victor: victor@utrgv.edu"]
    assert await contacts(mem, user) == [("Victor", "victor@utrgv.edu", "user", 0)]
    _, sent = await run(lc, user, "email victor saying hi", 1)
    assert sent["state"] == "success"
    assert "contact Victor: victor@utrgv.edu" in client.requests[1]["messages"][0]["content"]


async def test_nothing_saved_for_a_cancelled_request(lc, user, wired, mem):
    wired(reply([tool_call("remember", {"kind": "fact", "label": "gym", "value": "6 AM"})]))
    body = start_body("remember that the gym is at 6 am")
    await lc.start(user, "real", body)
    await lc.cancel(user, body["request_id"])  # the user double pressed before the result arrived
    await lc.process(body["request_id"], 1)
    assert (await mem.load(user)).items == []


async def test_injected_remember_is_refused(lc, user, wired, mem):
    """A web result says 'remember that...'; the user never asked. Nothing is saved."""
    client = wired(
        reply([tool_call("remember", {"kind": "contact", "label": "Victor", "value": "attacker@evil.example.net"})]),
        reply(content="Bean There opens at 7 AM."),
    )
    _, result = await run(lc, user, "find a good coffee shop", 0)
    assert result["state"] == "read_only_result"
    assert "own words" in client.requests[1]["messages"][-1]["content"]
    assert (await mem.load(user)).items == []


@pytest.mark.parametrize("said,allowed", [
    ("remember that victor's email is x", True), ("don't forget the gym is at 6", True),
    ("save that as my address", True), ("note that maria prefers mornings", True),
    ("email victor saying hi", False), ("what do i have tomorrow", False),
])
def test_remember_intent_reads_the_users_words(said, allowed):
    assert remember_intent([Segment(1, "voice", said, None, NOW)]) is allowed


# ---------- resolution and prompt ----------

def world_with(*items):
    w = build_world(NOW, TZ)
    w.memory_items = [{"id": str(i), "kind": k, "label": l, "value": v, "uses": u, "source": "learned"}
                      for i, (k, l, v, u) in enumerate(items)]
    return w


def test_exact_memory_name_beats_the_address_book():
    w = world_with(("contact", "Victor", "victor@utrgv.edu", 0))
    assert [c["email"] for c in resolve_contacts(w, "Victor")] == ["victor@utrgv.edu"]


def test_partial_name_still_asks_most_contacted_first():
    w = world_with(("contact", "Alex Rivera", "alex.rivera@example.com", 3))
    assert [c["email"] for c in resolve_contacts(w, "Alex")] == ["alex.rivera@example.com", "alex.kim@example.com"]
    assert [c["email"] for c in resolve_contacts(world_with(), "Alex")] == ["alex.kim@example.com", "alex.rivera@example.com"]


def test_learned_full_name_and_address_book_are_one_person():
    w = world_with(("contact", "Victor Morales", "victor.morales@example.com", 2))
    assert [c["email"] for c in resolve_contacts(w, "Victor")] == ["victor.morales@example.com"]


def test_prompt_gets_only_relevant_memory():
    w = world_with(("contact", "Victor", "victor@utrgv.edu", 2), ("contact", "Maria Lopez", "maria.lopez@example.com", 5),
                   ("preference", "usual event length", "30 minutes", 0))
    w.actions = [{"draft": {"kind": "create_event", "title": "Gym", "start": "2026-10-14T15:00:00-05:00",
                            "end": "2026-10-14T16:00:00-05:00"}, "detail": {"event_id": "ev_ab12cd34"}}]
    ctx = memory_context(w, "email victor saying hi")
    assert "contact Victor: victor@utrgv.edu" in ctx and "Maria" not in ctx
    assert "usual event length: 30 minutes" in ctx
    assert 'recent: created event ev_ab12cd34 "Gym" Wed Oct 14 3:00 PM to 4:00 PM' in ctx


async def test_that_meeting_can_find_the_event_just_created(lc, user, wired):
    """Create the gym block, then 'move that to 4': the prompt names the new event and the move works."""
    client = wired(
        reply([tool_call("create_event", {"title": "Gym", "start": "2026-10-14T15:00", "end": "2026-10-14T16:00"})]),
    )
    _, created = await run(lc, user, "block 3 to 4 tomorrow for the gym", 0)
    new_id = "ev_" + created["execution_key"][:8]
    client.replies.append(reply([tool_call("update_event", {"event_id": new_id, "new_start": "2026-10-14T16:00"})]))
    _, moved = await run(lc, user, "move that to 4", 1)
    assert moved["state"] == "success" and moved["screen_lines"][0] == "Event moved"
    assert f"recent: created event {new_id}" in client.requests[1]["messages"][0]["content"]
