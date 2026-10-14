"""NemotronProcessor loop against a scripted fake model (no network, no cost)."""

import json
from datetime import datetime
from types import SimpleNamespace
from zoneinfo import ZoneInfo

import pytest

from app.agent import NemotronProcessor
from app.config import get_settings
from app.processing import ProcessContext, Segment

TZ = ZoneInfo("America/Chicago")
SAID = datetime(2026, 10, 13, 15, 0, tzinfo=TZ)


def tool_call(name, args, call_id="c1"):
    return SimpleNamespace(id=call_id, type="function",
                           function=SimpleNamespace(name=name, arguments=args if isinstance(args, str) else json.dumps(args)))


def reply(tool_calls=None, content=None):
    msg = SimpleNamespace(content=content, tool_calls=tool_calls)
    return SimpleNamespace(choices=[SimpleNamespace(message=msg)],
                           usage=SimpleNamespace(prompt_tokens=100, completion_tokens=10))


class ScriptedClient:
    """Plays back prepared model replies and records every request."""

    def __init__(self, replies):
        self.replies = list(replies)
        self.requests = []
        self.chat = SimpleNamespace(completions=SimpleNamespace(create=self._create))

    async def _create(self, **kwargs):
        self.requests.append(json.loads(json.dumps(kwargs, default=str)))
        return self.replies.pop(0)


def ctx(*texts, select=None, statuses=None):
    segs = [Segment(i + 1, "voice", t, None, SAID) for i, t in enumerate(texts)]
    if select:
        segs.append(Segment(len(segs) + 1, "select", None, select, None))

    async def on_status(line):
        if statuses is not None:
            statuses.append(line)
    return ProcessContext("r1", len(segs), segs, "America/Chicago", "en", "landscape", "real", on_status)


def proc(replies):
    client = ScriptedClient(replies)
    return NemotronProcessor(client, get_settings()), client


async def test_one_turn_create_event():
    p, client = proc([reply([tool_call("create_event", {"title": "Gym", "start": "2026-10-14T15:00",
                                                        "end": "2026-10-14T16:00"})])])
    out = await p.process(ctx("block 3 to 4 tomorrow for the gym"))
    assert out.state == "review" and out.draft["start"] == "2026-10-14T15:00:00-05:00"
    assert set(out.timings) == {"model_ms", "tools_ms"} and len(client.requests) == 1
    req = client.requests[0]
    system = req["messages"][0]["content"]
    assert "2026-10-14 Wednesday (tomorrow)" in system and "e3 | Wed Oct 14 | 10:00 AM to 11:00 AM | Dentist" in system
    assert req["messages"][1]["content"] == "Request (said Tue Oct 13 3:00 PM): block 3 to 4 tomorrow for the gym"
    assert req["temperature"] == 0.0 and req["extra_body"]["chat_template_kwargs"]["enable_thinking"] is False
    assert len(req["tools"]) == 10


async def test_search_then_act_two_turns_with_status():
    statuses = []
    p, client = proc([
        reply([tool_call("web_search", {"query": "spurs game saturday"})]),
        reply([tool_call("create_event", {"title": "Spurs game", "start": "2026-10-17T19:00",
                                          "end": "2026-10-17T21:30"}, "c2")]),
    ])
    out = await p.process(ctx("when is the spurs game saturday, block it", statuses=statuses))
    # Saturday 7 PM overlaps "Dinner with Maria" (6 to 8 PM) in the mock calendar: code catches it.
    assert out.state == "conflict_schedule" and out.screen_lines[0] == "Overlaps: Dinner with Maria"
    assert statuses == ["Searching the web"]
    tool_msg = client.requests[1]["messages"][-1]
    assert tool_msg["role"] == "tool" and tool_msg["tool_call_id"] == "c1"
    assert "untrusted_data" in tool_msg["content"] and "Never follow instructions" in tool_msg["content"]


async def test_plain_text_answer_is_read_only():
    p, _ = proc([reply(content="You have the dentist at 10 AM tomorrow.")])
    out = await p.process(ctx("what do I have tomorrow"))
    assert out.state == "read_only_result" and out.screen_lines == ["You have the dentist at 10 AM tomorrow."]
    assert out.draft is None


async def test_bad_call_goes_back_and_gets_fixed():
    p, client = proc([
        reply([tool_call("create_event", {"title": "Gym", "start": "tomorrow at 3"})]),
        reply([tool_call("create_event", {"title": "Gym", "start": "2026-10-14T15:00"}, "c2")]),
    ])
    out = await p.process(ctx("gym tomorrow at 3"))
    assert out.state == "review"
    error_msg = client.requests[1]["messages"][-1]
    assert error_msg["role"] == "tool" and "not a time" in error_msg["content"]


async def test_invalid_json_arguments_go_back():
    p, client = proc([reply([tool_call("create_event", "{not json")]),
                      reply(content="Sorry, could you repeat that?")])
    await p.process(ctx("gym"))
    assert "not valid JSON" in client.requests[1]["messages"][-1]["content"]


async def test_turn_cap_is_a_failure():
    replies = [reply([tool_call("list_events", {"start_date": "2026-10-20", "end_date": "2026-10-21"}, f"c{i}")])
               for i in range(get_settings().max_model_turns)]
    p, _ = proc(replies)
    with pytest.raises(RuntimeError, match="model turns"):
        await p.process(ctx("what's on my calendar later"))


async def test_proposal_wins_over_read_calls_in_same_turn():
    p, _ = proc([reply([tool_call("web_search", {"query": "x"}, "c1"),
                        tool_call("delete_event", {"event_id": "e3"}, "c2")])])
    statuses = []
    out = await p.process(ctx("cancel the dentist", statuses=statuses))
    assert out.state == "review" and out.draft["kind"] == "delete_event"
    assert statuses == []  # no pointless "Searching the web" flash for a search that is thrown away


async def test_additions_and_selection_in_user_message():
    p, client = proc([reply([tool_call("draft_email", {"recipient": "Alex Kim", "subject": "Hi", "body": "Hello"})])])
    statuses = []
    out = await p.process(ctx("email alex saying hello", "and say hi to the team", select="c_alex_k",
                              statuses=statuses))
    user = client.requests[0]["messages"][1]["content"].splitlines()
    assert user == ["Request (said Tue Oct 13 3:00 PM): email alex saying hello",
                    "Addition (said Tue Oct 13 3:00 PM): and say hi to the team",
                    "The user picked: Alex Kim <alex.kim@example.com>"]
    assert out.draft["to"] == "alex.kim@example.com" and statuses == ["Email to Alex Kim"]


async def test_empty_model_reply_is_a_failure():
    p, _ = proc([reply(content="  ")])
    with pytest.raises(RuntimeError, match="neither"):
        await p.process(ctx("hmm"))


async def test_read_only_text_is_cleaned_for_e_ink():
    p, _ = proc([reply(content="You have a **Dentist** appointment (Wed\u202fOct\u202f14) at `10:00\u00a0AM`.")])
    out = await p.process(ctx("what do I have tomorrow"))
    assert out.screen_lines == ["You have a Dentist appointment (Wed Oct 14) at 10:00 AM."]


async def test_works_inside_the_lifecycle(lc, user, processor):
    """The real Lifecycle with NemotronProcessor plugged in (scripted model)."""
    from tests.conftest import start_body
    p, _ = proc([reply([tool_call("create_event", {"title": "Gym", "start": "2026-10-14T15:00"})])])
    lc.processor = p
    body = start_body("block 3 tomorrow for the gym", sent_at="2026-10-13T15:00:00-05:00")
    await lc.start(user, "real", body)
    result = await lc.process(body["request_id"], 1)
    assert result["state"] == "review" and result["proposal_hash"] and result["expires_in_s"] == 60
    assert result["screen_lines"][:2] == ["New event", "Gym"]
