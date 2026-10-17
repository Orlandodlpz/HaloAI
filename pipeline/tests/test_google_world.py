"""Real calendar and contacts as a world (B4 step 2), against a fake Google. No real Google calls."""

from datetime import date, datetime
from urllib.parse import parse_qs, urlparse
from zoneinfo import ZoneInfo

import httpx
import pytest

from app.google_auth import GoogleError, GoogleNotConnected
from app.google_world import (CONNECTIONS_URL, EVENTS_URL, EXPIRED, NO_ANSWER, NOT_CONNECTED, NOT_SET_UP,
                              OTHER_CONTACTS_URL, GoogleWorld, event_alias)
from app.processing import ProcessorOutput, ScreenError
from app.prompt import system_prompt
from app.tools import Final, ToolError, run_tool
from tests.conftest import start_body

TZ = ZoneInfo("America/Chicago")
NOW = datetime(2026, 10, 16, 9, 0, tzinfo=TZ)  # a Friday

EVENTS = [
    {"id": "timed1abcdefghijklmnopqrstu", "summary": "Office hours", "description": "Room 2.104", "etag": '"v1"',
     "organizer": {"self": True}, "attendees": [{"email": "me@example.com", "self": True},
                                                {"email": "ta@uni.example.edu"}, {"email": "room@resource.example.com",
                                                                                  "resource": True}],
     "start": {"dateTime": "2026-10-17T15:00:00Z"}, "end": {"dateTime": "2026-10-17T16:00:00Z"}},
    {"id": "allday1", "summary": "Fall break", "start": {"date": "2026-10-19"}, "end": {"date": "2026-10-20"},
     "transparency": "transparent"},
    {"id": "busyday1", "summary": "Hackathon", "start": {"date": "2026-10-21"}, "end": {"date": "2026-10-22"}},
    {"id": "weekly1_20261020T140000Z", "summary": "Class", "recurringEventId": "weekly1",
     "start": {"dateTime": "2026-10-20T09:00:00-05:00"}, "end": {"dateTime": "2026-10-20T10:15:00-05:00"}},
    {"id": "declined1", "summary": "Optional talk", "attendees": [{"self": True, "responseStatus": "declined"}],
     "start": {"dateTime": "2026-10-17T12:00:00-05:00"}, "end": {"dateTime": "2026-10-17T13:00:00-05:00"}},
    {"id": "gone1", "status": "cancelled", "summary": "Cancelled thing",
     "start": {"dateTime": "2026-10-17T08:00:00-05:00"}, "end": {"dateTime": "2026-10-17T09:00:00-05:00"}},
]
SAVED = [{"names": [{"displayName": "Carlos Torres"}], "emailAddresses": [{"value": "carlos@uni.example.edu"}]},
         {"names": [{"displayName": "Orlando De Leon"}], "emailAddresses": [{"value": "orlando@uni.example.edu"}]}]
OTHER = [{"names": [{"displayName": "Carlos T"}], "emailAddresses": [{"value": "CARLOS@uni.example.edu"}]},
         {"emailAddresses": [{"value": "noname@example.com"}]}]


class FakeGoogleApi:
    def __init__(self):
        self.calls: list[tuple[str, dict]] = []
        self.events = list(EVENTS)
        self.status = 200
        self.unauthorized = 0  # answer 401 this many times first
        self.event_pages = None  # list of item lists, to test paging

    def handler(self, request: httpx.Request) -> httpx.Response:
        url = str(request.url).split("?")[0]
        params = {k: v[0] for k, v in parse_qs(urlparse(str(request.url)).query).items()}
        self.calls.append((url, params))
        if self.unauthorized:
            self.unauthorized -= 1
            return httpx.Response(401)
        if self.status != 200:
            return httpx.Response(self.status)
        if url == EVENTS_URL:
            if self.event_pages:
                i = int(params.get("pageToken", "0"))
                more = {"nextPageToken": str(i + 1)} if i + 1 < len(self.event_pages) else {}
                return httpx.Response(200, json={"items": self.event_pages[i], **more})
            return httpx.Response(200, json={"items": self.events})
        if url == CONNECTIONS_URL:
            return httpx.Response(200, json={"connections": SAVED})
        if url == OTHER_CONTACTS_URL:
            return httpx.Response(200, json={"otherContacts": OTHER})
        return httpx.Response(404)

    def count(self, url):
        return sum(1 for u, _ in self.calls if u == url)


class StubAuth:
    def __init__(self):
        self.error: Exception | None = None
        self.ok = True
        self.dropped = 0
        self.on_change: list = []

    def configured(self):
        return self.ok

    async def access_token(self, user_id):
        if self.error:
            raise self.error
        return "tok"

    def drop_access_token(self, user_id):
        self.dropped += 1


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


@pytest.fixture
def api():
    return FakeGoogleApi()


@pytest.fixture
def stub_auth():
    return StubAuth()


@pytest.fixture
def mono():
    return Clock()


@pytest.fixture
def gworld(settings, api, stub_auth, mono):
    return GoogleWorld(stub_auth, settings, httpx.AsyncClient(transport=httpx.MockTransport(api.handler)),
                       monotonic=mono)


async def world(gworld):
    return await gworld.world("u1", NOW, TZ)


def ev(w, title):
    return next(e for e in w.events if e["title"] == title)


# ---------- reading ----------

async def test_events_get_short_stable_aliases_and_keep_googles_id(gworld):
    w = await world(gworld)
    assert w.source == "google" and w.window == (date(2026, 10, 9), date(2026, 12, 15))
    office = ev(w, "Office hours")
    assert office["event_id"] == event_alias("timed1abcdefghijklmnopqrstu") and len(office["event_id"]) == 8
    assert office["google_id"] == "timed1abcdefghijklmnopqrstu" and office["notes"] == "Room 2.104"
    assert office["start"] == datetime(2026, 10, 17, 10, 0, tzinfo=TZ)  # UTC from Google, local for the model
    again = await world(gworld)
    assert [e["event_id"] for e in again.events] == [e["event_id"] for e in w.events]  # same alias every request


async def test_all_day_free_declined_and_cancelled(gworld):
    w = await world(gworld)
    titles = {e["title"] for e in w.events}
    assert "Cancelled thing" not in titles and {"Fall break", "Hackathon", "Optional talk", "Class"} <= titles
    brk = ev(w, "Fall break")
    assert brk["all_day"] and brk["start"] == datetime(2026, 10, 19, tzinfo=TZ)
    assert brk["end"] == datetime(2026, 10, 20, tzinfo=TZ) and brk["busy"] is False  # shown as free
    assert ev(w, "Hackathon")["busy"] is True        # all day but busy: it does block
    assert ev(w, "Optional talk")["busy"] is False   # declined: never blocks
    assert ev(w, "Class")["google_id"] == "weekly1_20261020T140000Z"  # one occurrence of the weekly event


async def test_reads_the_window_with_single_events(gworld, api):
    await world(gworld)
    _, params = next(c for c in api.calls if c[0] == EVENTS_URL)
    assert params["timeMin"] == "2026-10-09T00:00:00-05:00" and params["timeMax"] == "2026-12-16T00:00:00-06:00"
    assert params["singleEvents"] == "true" and params["orderBy"] == "startTime"


async def test_event_pages_are_followed(gworld, api):
    api.event_pages = [[EVENTS[0]], [EVENTS[2]]]
    w = await world(gworld)
    assert [e["title"] for e in w.events] == ["Office hours", "Hackathon"]


async def test_contacts_merge_saved_and_other_without_duplicates(gworld):
    w = await world(gworld)
    by_email = {c["email"].lower(): c for c in w.contacts}
    assert len(w.contacts) == 3
    assert by_email["carlos@uni.example.edu"]["name"] == "Carlos Torres"  # saved contact wins
    assert by_email["noname@example.com"]["name"] == "noname@example.com"  # reachable by address only
    assert all(c["contact_id"].startswith("g_") for c in w.contacts)
    assert all("example.com" not in c["email"] or c["email"] == "noname@example.com" for c in w.contacts)


async def test_contacts_are_cached_calendar_is_not(gworld, api, mono, stub_auth):
    await world(gworld)
    await world(gworld)
    assert api.count(EVENTS_URL) == 2 and api.count(CONNECTIONS_URL) == 1
    mono.t += 601
    await world(gworld)
    assert api.count(CONNECTIONS_URL) == 2
    for listener in stub_auth.on_change:  # account connected again or removed
        listener("u1")
    await world(gworld)
    assert api.count(CONNECTIONS_URL) == 3


# ---------- errors become clear screens, never sample data ----------

@pytest.mark.parametrize("error,lines", [
    (GoogleNotConnected("No Google account connected"), NOT_CONNECTED),
    (GoogleError("The Google sign in expired. Connect the account again from the app."), EXPIRED),
])
async def test_sign_in_problems(gworld, stub_auth, error, lines):
    stub_auth.error = error
    with pytest.raises(ScreenError) as exc:
        await world(gworld)
    assert exc.value.lines == lines


async def test_not_set_up(gworld, stub_auth):
    stub_auth.ok = False
    with pytest.raises(ScreenError) as exc:
        await world(gworld)
    assert exc.value.lines == NOT_SET_UP


async def test_google_down(gworld, api):
    api.status = 503
    with pytest.raises(ScreenError) as exc:
        await world(gworld)
    assert exc.value.lines == NO_ANSWER


async def test_refused_token_is_refreshed_once(gworld, api, stub_auth):
    api.unauthorized = 1
    w = await world(gworld)
    assert stub_auth.dropped == 1 and w.events
    api.unauthorized = 10
    with pytest.raises(ScreenError) as exc:
        await world(gworld)
    assert exc.value.lines == EXPIRED


# ---------- the tools on a real world ----------

async def test_free_and_declined_events_never_block_but_busy_all_day_does(gworld):
    w = await world(gworld)
    free = run_tool("create_event", {"title": "Gym", "start": "2026-10-19T15:00"}, w, NOW)
    declined = run_tool("create_event", {"title": "Lunch", "start": "2026-10-17T12:00"}, w, NOW)
    busy = run_tool("create_event", {"title": "Gym", "start": "2026-10-21T15:00"}, w, NOW)
    assert free.output.state == declined.output.state == "review"
    assert busy.output.state == "conflict_schedule"
    assert busy.output.screen_lines[:2] == ["You already have Hackathon then", "Wed Oct 21, all day"]


async def test_past_the_window_says_so_instead_of_calling_it_free(gworld):
    w = await world(gworld)
    far = run_tool("create_event", {"title": "Trip", "start": "2026-12-20T10:00"}, w, NOW)
    assert isinstance(far, Final) and far.output.state == "read_only_result" and far.output.draft is None
    assert far.output.screen_lines[0] == "I can only plan up to Tue Dec 15"
    office = ev(w, "Office hours")["event_id"]
    moved = run_tool("update_event", {"event_id": office, "new_start": "2026-12-20T10:00"}, w, NOW)
    assert moved.output.state == "read_only_result" and moved.output.draft is None


async def test_list_events_is_clipped_to_the_window(gworld):
    w = await world(gworld)
    got = run_tool("list_events", {"start_date": "2026-12-01", "end_date": "2026-12-31"}, w, NOW)
    assert got.data == {"events": []}
    with pytest.raises(ToolError, match="only be read from 2026-10-09 to 2026-12-15"):
        run_tool("list_events", {"start_date": "2027-01-01", "end_date": "2027-01-05"}, w, NOW)


async def test_drafts_carry_googles_id_and_all_day_cannot_be_moved(gworld):
    w = await world(gworld)
    office = ev(w, "Office hours")
    moved = run_tool("update_event", {"event_id": office["event_id"], "new_start": "2026-10-17T11:00"}, w, NOW)
    gone = run_tool("delete_event", {"event_id": office["event_id"]}, w, NOW)
    assert moved.output.draft["google_id"] == gone.output.draft["google_id"] == "timed1abcdefghijklmnopqrstu"
    assert moved.output.draft["etag"] == '"v1"' and moved.output.draft["guest_emails"] == ["ta@uni.example.edu"]
    with pytest.raises(ToolError, match="all day"):
        run_tool("update_event", {"event_id": ev(w, "Hackathon")["event_id"], "new_start": "2026-10-22T09:00"}, w, NOW)
    deleted_day = run_tool("delete_event", {"event_id": ev(w, "Hackathon")["event_id"]}, w, NOW)
    assert deleted_day.output.screen_lines[2] == "Wed Oct 21 all day"


async def test_web_search_is_not_faked_on_the_real_world(gworld):
    w = await world(gworld)
    with pytest.raises(ToolError, match="not connected"):
        run_tool("web_search", {"query": "spurs game"}, w, NOW)


async def test_prompt_marks_all_day_and_free_events(gworld):
    w = await world(gworld)
    prompt = system_prompt(NOW, "America/Chicago", w, 200)
    alias = ev(w, "Fall break")["event_id"]
    assert f"{alias} | Mon Oct 19 | all day | Fall break (free)" in prompt
    assert "timed1abcdefghijklmnopqrstu" not in prompt  # the model only sees aliases


async def test_agent_stamps_the_source_on_real_drafts(gworld):
    from app.agent import NemotronProcessor
    from app.config import get_settings
    from tests.test_agent import ScriptedClient, ctx, reply, tool_call

    async def factory(c, now, tz):
        return await gworld.world("u1", now, tz)
    client = ScriptedClient([reply([tool_call("create_event", {"title": "Gym", "start": "2026-10-14T15:00"})])])
    out = await NemotronProcessor(client, get_settings(), world_factory=factory).process(ctx("gym at 3 tomorrow"))
    assert out.state == "review" and out.draft["source"] == "google"
    client = ScriptedClient([reply([tool_call("create_event", {"title": "Gym", "start": "2026-10-14T15:00"})])])
    sample = await NemotronProcessor(client, get_settings()).process(ctx("gym at 3 tomorrow"))
    assert "source" not in sample.draft  # sample drafts unchanged


# ---------- which world, which executor ----------

class DraftProcessor:
    """Returns one review with the given draft, or raises the given error."""

    def __init__(self, draft=None, error=None):
        self.draft, self.error = draft, error

    async def process(self, c):
        if self.error:
            raise self.error
        return ProcessorOutput("review", ["New event", "Gym"], draft=dict(self.draft))


GOOGLE_DRAFT = {"kind": "create_event", "title": "Gym", "start": "2026-10-14T15:00:00-05:00", "source": "google"}
SAMPLE_DRAFT = {"kind": "create_event", "title": "Gym", "start": "2026-10-14T15:00:00-05:00"}


async def approve(lc, user, scope="real"):
    body = start_body("gym")
    rev = await lc.start(user, scope, body)
    r = await lc.process(body["request_id"], rev)
    return await lc.approve(user, body["request_id"], {"revision": r["revision"], "proposal_hash": r["proposal_hash"]})


async def test_real_draft_goes_to_the_real_executor(lc, user, executors, settings):
    lc.settings = settings.model_copy(update={"real_tools_enabled": True})
    lc.processor = DraftProcessor(GOOGLE_DRAFT)
    await approve(lc, user)
    assert len(executors["real"].calls) == 1 and executors["mock"].calls == []


async def test_real_draft_is_refused_when_real_tools_were_switched_off(lc, user, executors):
    lc.processor = DraftProcessor(GOOGLE_DRAFT)  # real_tools_enabled is off
    r = await approve(lc, user)
    assert executors["real"].calls == executors["mock"].calls == []
    assert r["state"] == "failure" and r["screen_lines"] == ["Not done", "Real Google actions are switched off"]


async def test_real_draft_from_a_demo_key_is_refused(lc, demo_user, executors, settings):
    lc.settings = settings.model_copy(update={"real_tools_enabled": True})
    lc.processor = DraftProcessor(GOOGLE_DRAFT)
    await approve(lc, demo_user, scope="mock")
    assert executors["real"].calls == executors["mock"].calls == []


async def test_sample_draft_never_reaches_the_real_executor(lc, user, executors, settings):
    lc.settings = settings.model_copy(update={"real_tools_enabled": True})
    lc.processor = DraftProcessor(SAMPLE_DRAFT)
    await approve(lc, user)
    assert executors["real"].calls == [] and len(executors["mock"].calls) == 1


async def test_screen_error_shows_its_own_lines(lc, user):
    lc.processor = DraftProcessor(error=ScreenError(NOT_CONNECTED))
    body = start_body("what do I have tomorrow")
    rev = await lc.start(user, "real", body)
    r = await lc.process(body["request_id"], rev)
    assert r["state"] == "failure" and r["screen_lines"] == NOT_CONNECTED


@pytest.mark.parametrize("scope,enabled,expected", [
    ("real", True, "google"), ("real", False, "sample"), ("mock", True, "sample"), ("mock", False, "sample")])
async def test_world_source_follows_the_same_rule(settings, scope, enabled, expected):
    from types import SimpleNamespace

    from app.main import world_source
    from app.memory import MemorySnapshot
    from app.mockdata import build_world

    class Source:
        def __init__(self, name):
            self.name = name

        async def world(self, user_id, now, tz):
            w = build_world(now, tz)
            w.source = self.name
            return w

    class Memory:
        async def load(self, user_id):
            return MemorySnapshot(items=[], actions=[])

    s = settings.model_copy(update={"real_tools_enabled": enabled})
    factory = world_source(s, Source("sample"), Memory(), Source("google"))
    w = await factory(SimpleNamespace(scope=scope, user_id="u1"), NOW, TZ)
    assert w.source == expected
