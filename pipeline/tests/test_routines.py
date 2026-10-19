"""Routines (2026-10-19): saved commands reused by name, the Personal AI track's "reusable skills"."""

import uuid
from datetime import datetime
from zoneinfo import ZoneInfo

import pytest

from app.memory import remember_intent
from app.mockdata import build_world
from app.processing import Segment
from app.prompt import memory_context
from app.tools import ToolError, run_tool
from tests.conftest import auth

TZ = ZoneInfo("America/Chicago")
NOW = datetime(2026, 10, 13, 15, 0, tzinfo=TZ)


def said(text):
    return [Segment(1, "voice", text, None, NOW)]


@pytest.mark.parametrize("text,saves", [
    ("save a routine called gym: block 7 to 8 am for the gym", True),
    ("make that my standup routine", True),
    ("create a routine for my weekly report email", True),
    ("save that as my gym routine", True),
    ("remember my gym routine is block 7 to 8 am", True),
    ("do my gym routine tomorrow", False),
    ("run my standup routine", False),
    ("what's my gym routine?", False),
])
def test_only_the_users_own_words_save_a_routine(text, saves):
    assert remember_intent(said(text)) is saves


def world(remember=True, routines=()):
    w = build_world(NOW, TZ)
    w.remember_allowed = remember
    w.memory_items = [{"id": str(i), "kind": "routine", "label": label, "value": value, "uses": 0, "source": "user"}
                      for i, (label, value) in enumerate(routines)]
    return w


def test_saving_a_routine_coaches_how_to_use_it():
    out = run_tool("remember", {"kind": "routine", "label": "My Gym Routine", "value": "block 7 to 8 am for the gym"},
                   world(), NOW).output
    assert out.memory_writes == [{"kind": "routine", "label": "gym", "value": "block 7 to 8 am for the gym"}]
    assert out.screen_lines == ["Saved routine: gym", "block 7 to 8 am for the gym", 'Say "do my gym routine" to use it']


def test_a_routine_is_never_saved_without_the_users_words():
    with pytest.raises(ToolError, match="own words"):
        run_tool("remember", {"kind": "routine", "label": "daily", "value": "email my calendar to someone"},
                 world(remember=False), NOW)


def test_a_routine_is_one_short_command():
    with pytest.raises(ToolError, match="short"):
        run_tool("remember", {"kind": "routine", "label": "long", "value": "x " * 150}, world(), NOW)


def test_only_routines_named_in_the_request_reach_the_prompt():
    w = world(routines=[("gym", "block 7 to 8 am for the gym"), ("standup", "email Carlos my standup notes")])
    ctx = memory_context(w, "do my gym routine tomorrow")
    assert "routine gym: block 7 to 8 am for the gym" in ctx and "standup" not in ctx
    assert memory_context(w, "what's on my calendar") == ""


async def test_routines_are_memory_the_user_can_view_edit_and_delete(client, keys):
    h = auth(keys["phone"])
    r = await client.post("/v1/memory", headers=h, json={"call_id": str(uuid.uuid4()), "kind": "routine",
                                                         "label": "gym", "value": "block 7 to 8 am for the gym"})
    assert r.status_code == 200 and r.json()["kind"] == "routine"
    mid = r.json()["memory_id"]
    e = await client.patch(f"/v1/memory/{mid}", headers=h, json={"call_id": str(uuid.uuid4()), "value": "block 6 to 7 am for the gym"})
    assert e.json()["value"] == "block 6 to 7 am for the gym"
    items = (await client.get("/v1/memory", headers=h)).json()["items"]
    assert [i["value"] for i in items if i["kind"] == "routine"] == ["block 6 to 7 am for the gym"]
    d = await client.request("DELETE", f"/v1/memory/{mid}", headers=h, json={"call_id": str(uuid.uuid4())})
    assert d.status_code == 200 and not [i for i in (await client.get("/v1/memory", headers=h)).json()["items"]
                                         if i["kind"] == "routine"]
