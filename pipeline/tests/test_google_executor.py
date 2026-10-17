"""Real actions (B4 step 3) against a fake Google that records every call. No real Google calls."""

import base64
import email
import email.policy
import json

import httpx
import pytest
from pydantic import SecretStr

from app.google_auth import GoogleError
from app.google_executor import EVENTS_URL, SEND_URL, GoogleExecutor, created_google_id
from app.google_world import EXPIRED, GoogleWorld, event_alias
from tests.conftest import start_body
from tests.test_google_world import FakeGoogleApi, StubAuth

KEY = "abcdefghijklmnopqrstuvwxyz"
ALLOWED = "carlos@uni.example.edu,ta@uni.example.edu"


class FakeGoogleWrites(FakeGoogleApi):
    """Reads as in test_google_world, plus the write endpoints. `answers` overrides status codes
    per (method, url prefix), as a list consumed one call at a time."""

    def __init__(self):
        super().__init__()
        self.writes: list[dict] = []
        self.answers: dict[tuple[str, str], list] = {}

    def handler(self, request: httpx.Request) -> httpx.Response:
        url = str(request.url).split("?")[0]
        if request.method == "GET":
            for (method, prefix), queue in self.answers.items():
                if method == "GET" and url.startswith(prefix) and queue:
                    return self._answer(queue.pop(0), request)
            return super().handler(request)
        self.writes.append({"method": request.method, "url": url, "params": dict(request.url.params),
                            "headers": dict(request.headers),
                            "json": json.loads(request.content) if request.content else None})
        for (method, prefix), queue in self.answers.items():
            if method == request.method and url.startswith(prefix) and queue:
                return self._answer(queue.pop(0), request)
        if url == SEND_URL:
            return httpx.Response(200, json={"id": "m1"})
        if request.method == "POST" and url == EVENTS_URL:
            body = json.loads(request.content)
            out = {**body, "status": "confirmed"}
            if "conferenceData" in body:
                out["hangoutLink"] = "https://meet.google.com/abc-defg-hij"
            return httpx.Response(200, json=out)
        if request.method == "PATCH":
            return httpx.Response(200, json={"id": url.rsplit("/", 1)[1]})
        if request.method == "DELETE":
            return httpx.Response(204)
        return httpx.Response(404)

    @staticmethod
    def _answer(answer, request):
        if isinstance(answer, Exception):
            raise answer
        code, body = answer if isinstance(answer, tuple) else (answer, {})
        return httpx.Response(code, json=body)

    def gmail_calls(self):
        return [w for w in self.writes if w["url"] == SEND_URL]


@pytest.fixture
def fake():
    return FakeGoogleWrites()


@pytest.fixture
def stub_auth():
    return StubAuth()


@pytest.fixture
def real_settings(settings):
    return settings.model_copy(update={"allowed_recipients": SecretStr(ALLOWED)})


@pytest.fixture
def executor(stub_auth, real_settings, fake):
    return GoogleExecutor(stub_auth, real_settings, httpx.AsyncClient(transport=httpx.MockTransport(fake.handler)))


def create_draft(**extra):
    return {"kind": "create_event", "source": "google", "title": "Call with Carlos",
            "start": "2026-10-20T15:00:00-05:00", "end": "2026-10-20T16:00:00-05:00",
            "timezone": "America/Chicago", "guests": [], "video_call": False, "description": "", **extra}


def update_draft(**extra):
    return {"kind": "update_event", "source": "google", "event_id": "g1234567", "google_id": "gid1", "etag": '"v1"',
            "guest_emails": [], "timezone": "America/Chicago",
            "before": {"title": "Office hours"},
            "after": {"title": "Office hours", "start": "2026-10-17T11:00:00-05:00", "end": "2026-10-17T12:00:00-05:00"},
            **extra}


def email_draft(**extra):
    return {"kind": "send_email", "source": "google", "to": "carlos@uni.example.edu", "to_name": "Carlos",
            "subject": "Demo", "body": "The demo moved to Thursday.", **extra}


# ---------- calendar ----------

async def test_create_with_guests_and_meet(executor, fake):
    out = await executor.execute(create_draft(guests=[{"name": "Carlos", "email": "carlos@uni.example.edu"}],
                                              video_call=True, description="Budget"), KEY, "u1")
    (call,) = fake.writes
    assert call["params"] == {"conferenceDataVersion": "1", "sendUpdates": "all"}
    body = call["json"]
    assert body["id"] == created_google_id(KEY) and body["summary"] == "Call with Carlos"
    assert body["start"] == {"dateTime": "2026-10-20T15:00:00-05:00", "timeZone": "America/Chicago"}
    assert body["attendees"] == [{"email": "carlos@uni.example.edu"}] and body["description"] == "Budget"
    assert body["conferenceData"]["createRequest"]["requestId"] == KEY  # a repeat reuses the same Meet link
    assert out.applied and out.screen_lines == ["Added to your calendar", "Call with Carlos", "Tue Oct 20, 3:00 PM to 4:00 PM",
                                                "Invitations sent", "Google Meet link added"]
    assert out.detail["event_id"] == event_alias(created_google_id(KEY))  # "move that meeting" finds it


async def test_create_alone_sends_no_invitations(executor, fake):
    out = await executor.execute(create_draft(), KEY, "u1")
    (call,) = fake.writes
    assert call["params"]["sendUpdates"] == "none" and "attendees" not in call["json"]
    assert "conferenceData" not in call["json"] and out.screen_lines[-1] == "Tue Oct 20, 3:00 PM to 4:00 PM"


async def test_event_id_is_valid_for_google_and_stable():
    gid = created_google_id(KEY)
    assert gid == created_google_id(KEY) and 5 <= len(gid) <= 1024
    assert set(gid) <= set("abcdefghijklmnopqrstuv0123456789")  # Google's allowed characters


async def test_repeated_create_finds_its_own_event(executor, fake):
    fake.answers[("POST", EVENTS_URL)] = [(409, {})]
    fake.answers[("GET", EVENTS_URL)] = [(200, {"id": created_google_id(KEY), "status": "confirmed"})]
    assert (await executor.execute(create_draft(), KEY, "u1")).applied
    fake.answers[("POST", EVENTS_URL)] = [(409, {})]
    fake.answers[("GET", EVENTS_URL)] = [(200, {"status": "cancelled"})]
    out = await executor.execute(create_draft(), KEY, "u1")
    assert not out.applied and out.screen_lines[0] == "Not added"


async def test_guest_off_the_allowlist_is_refused_before_any_call(executor, fake):
    out = await executor.execute(create_draft(guests=[{"name": "x", "email": "stranger@example.net"}]), KEY, "u1")
    assert not out.applied and "stranger@example.net" in out.screen_lines[1] and fake.writes == []


async def test_move_uses_the_version_seen_in_review(executor, fake):
    out = await executor.execute(update_draft(), KEY, "u1")
    (call,) = fake.writes
    assert call["method"] == "PATCH" and call["url"] == f"{EVENTS_URL}/gid1"
    assert call["headers"]["if-match"] == '"v1"' and call["params"]["sendUpdates"] == "none"
    assert out.applied and out.screen_lines[0] == "Event moved" and out.detail == {"event_id": "g1234567"}


async def test_guests_hear_about_a_move_only_if_all_are_allowed(executor, fake):
    await executor.execute(update_draft(guest_emails=["TA@uni.example.edu"]), KEY, "u1")
    await executor.execute(update_draft(guest_emails=["ta@uni.example.edu", "stranger@example.net"]), KEY, "u1")
    assert [w["params"]["sendUpdates"] for w in fake.writes] == ["all", "none"]


@pytest.mark.parametrize("code,line", [(412, "That event changed since you saw it"),
                                       (404, "That event is no longer on your calendar")])
async def test_move_refused_when_the_event_changed_or_is_gone(executor, fake, code, line):
    fake.answers[("PATCH", EVENTS_URL)] = [(code, {})]
    out = await executor.execute(update_draft(), KEY, "u1")
    assert not out.applied and out.screen_lines == ["Not changed", line]


async def test_delete(executor, fake):
    draft = {"kind": "delete_event", "source": "google", "event_id": "g1234567", "google_id": "gid1", "etag": '"v1"',
             "guest_emails": [], "event": {"title": "Office hours"}}
    out = await executor.execute(draft, KEY, "u1")
    assert out.applied and fake.writes[0]["method"] == "DELETE" and fake.writes[0]["headers"]["if-match"] == '"v1"'
    fake.answers[("DELETE", EVENTS_URL)] = [(410, {})]
    assert (await executor.execute(draft, KEY, "u1")).applied  # already gone counts as done
    fake.answers[("DELETE", EVENTS_URL)] = [(412, {})]
    changed = await executor.execute(draft, KEY, "u1")
    assert not changed.applied and changed.screen_lines[1] == "That event changed since you saw it"


# ---------- email ----------

async def test_send_email(executor, fake):
    out = await executor.execute(email_draft(subject="Demo\nBcc: attacker@evil.example.net"), KEY, "u1")
    (call,) = fake.gmail_calls()
    msg = email.message_from_bytes(base64.urlsafe_b64decode(call["json"]["raw"]), policy=email.policy.default)
    assert msg["To"] == "carlos@uni.example.edu" and msg["Bcc"] is None  # no header injection
    assert msg["Subject"] == "Demo Bcc: attacker@evil.example.net" and msg["Message-ID"] == f"<{KEY}@haloai-cov.io>"
    assert msg.get_content().strip() == "The demo moved to Thursday."
    assert out.applied and out.screen_lines[:2] == ["Email sent", "To: carlos@uni.example.edu"]


@pytest.mark.parametrize("to", ["stranger@example.net", "not an address"])
async def test_email_off_the_allowlist_or_invalid_is_never_sent(executor, fake, to):
    out = await executor.execute(email_draft(to=to), KEY, "u1")
    assert not out.applied and out.screen_lines[0] == "Not sent" and fake.gmail_calls() == []


async def test_no_allowlist_means_nobody(stub_auth, settings, fake):
    ex = GoogleExecutor(stub_auth, settings.model_copy(update={"allowed_recipients": None}),
                        httpx.AsyncClient(transport=httpx.MockTransport(fake.handler)))
    assert not (await ex.execute(email_draft(), KEY, "u1")).applied and fake.gmail_calls() == []


async def test_gmail_refusal_is_not_sent(executor, fake):
    fake.answers[("POST", SEND_URL)] = [(400, {})]
    out = await executor.execute(email_draft(), KEY, "u1")
    assert not out.applied and out.screen_lines[0] == "Not sent"


@pytest.mark.parametrize("answer", [(500, {}), (503, {}), httpx.ReadTimeout("slow"), httpx.ConnectError("down")])
async def test_unknown_outcomes_raise_so_they_become_uncertain(executor, fake, answer):
    fake.answers[("POST", SEND_URL)] = [answer]
    with pytest.raises(Exception):
        await executor.execute(email_draft(), KEY, "u1")


# ---------- tokens and sources ----------

async def test_refused_token_is_retried_once_with_a_fresh_one(executor, fake, stub_auth):
    fake.answers[("POST", SEND_URL)] = [(401, {})]
    assert (await executor.execute(email_draft(), KEY, "u1")).applied and stub_auth.dropped == 1
    fake.answers[("POST", SEND_URL)] = [(401, {}), (401, {})]
    out = await executor.execute(email_draft(), KEY, "u1")
    assert not out.applied and out.screen_lines == ["Not done", *EXPIRED]


async def test_sign_in_expired_is_not_done(executor, fake, stub_auth):
    stub_auth.error = GoogleError("expired")
    out = await executor.execute(email_draft(), KEY, "u1")
    assert not out.applied and out.screen_lines == ["Not done", *EXPIRED] and fake.writes == []


async def test_sample_drafts_are_refused(executor, fake):
    draft = email_draft()
    del draft["source"]
    assert not (await executor.execute(draft, KEY, "u1")).applied and fake.writes == []


# ---------- the whole path: Gmail is never contacted before approval ----------

@pytest.fixture
def real_lc(lc, executors, real_settings, stub_auth, fake):
    from app.agent import NemotronProcessor
    from tests.test_agent import ScriptedClient, reply, tool_call
    http = httpx.AsyncClient(transport=httpx.MockTransport(fake.handler))
    gworld = GoogleWorld(stub_auth, real_settings, http)

    async def factory(ctx, now, tz):
        return await gworld.world(ctx.user_id, now, tz)

    lc.settings = real_settings.model_copy(update={"real_tools_enabled": True})
    executors["real"] = GoogleExecutor(stub_auth, real_settings, http)

    def script(*calls):
        lc.processor = NemotronProcessor(ScriptedClient([reply([tool_call(n, a)]) for n, a in calls]),
                                         lc.settings, world_factory=factory)
    lc.script = script
    return lc


async def review(lc, user, text):
    body = start_body(text, sent_at="2026-10-16T09:00:00-05:00")
    rev = await lc.start(user, "real", body)
    return body["request_id"], await lc.process(body["request_id"], rev)


async def test_gmail_is_contacted_only_after_approval(real_lc, user, fake):
    real_lc.script(("draft_email", {"recipient": "Carlos", "subject": "Demo", "body": "Moved to Thursday."}))
    rid, r = await review(real_lc, user, "email carlos the demo moved to thursday")
    assert r["state"] == "review" and r["draft"]["source"] == "google"
    assert fake.gmail_calls() == []  # REVIEW on the device, nothing sent
    done = await real_lc.approve(user, rid, {"revision": r["revision"], "proposal_hash": r["proposal_hash"]})
    assert done["state"] == "success" and done["screen_lines"][0] == "Email sent"
    again = await real_lc.approve(user, rid, {"revision": r["revision"], "proposal_hash": r["proposal_hash"]})
    assert again["state"] == "success" and len(fake.gmail_calls()) == 1  # exactly once


async def test_cancelled_and_expired_drafts_leave_nothing(real_lc, user, fake, clock):
    real_lc.script(("draft_email", {"recipient": "Carlos", "subject": "A", "body": "One."}))
    rid, _ = await review(real_lc, user, "email carlos one")
    await real_lc.cancel(user, rid)
    real_lc.script(("draft_email", {"recipient": "Carlos", "subject": "B", "body": "Two."}))
    rid2, r2 = await review(real_lc, user, "email carlos two")
    clock.advance(61)
    with pytest.raises(Exception):
        await real_lc.approve(user, rid2, {"revision": r2["revision"], "proposal_hash": r2["proposal_hash"]})
    assert fake.writes == []


async def test_stranger_is_refused_before_review(real_lc, user, fake):
    real_lc.script(("draft_email", {"recipient": "stranger at example dot net", "subject": "Hi", "body": "Hello."}))
    _, r = await review(real_lc, user, "email stranger at example dot net saying hello")
    assert r["state"] == "read_only_result" and r["proposal_hash"] is None
    assert r["screen_lines"] == ["I can't email stranger@example.net yet", "Only team addresses work in the demo"]
    assert fake.writes == []


async def test_uncertain_email_says_where_to_check(real_lc, user, fake):
    fake.answers[("POST", SEND_URL)] = [httpx.ReadTimeout("slow")]
    real_lc.script(("draft_email", {"recipient": "Carlos", "subject": "Demo", "body": "Moved."}))
    rid, r = await review(real_lc, user, "email carlos moved")
    done = await real_lc.approve(user, rid, {"revision": r["revision"], "proposal_hash": r["proposal_hash"]})
    assert done["state"] == "execution_uncertain" and done["failure_stage"] is None
    assert done["screen_lines"][:2] == ["Not sure it was sent", "Check Sent in Gmail before trying again"]


async def test_invite_off_the_allowlist_is_refused_before_review(real_lc, user, fake):
    real_lc.script(("create_event", {"title": "Call", "start": "2026-10-20T15:00",
                                     "guests": ["stranger@example.net"], "video_call": True}))
    _, r = await review(real_lc, user, "call with stranger@example.net tuesday at 3")
    assert r["state"] == "read_only_result" and r["screen_lines"][0].startswith("I can't invite")
    assert fake.writes == []


async def test_someone_elses_event_cannot_be_moved(real_lc, user, fake):
    fake.events = [{"id": "inv1", "summary": "Advisor meeting", "organizer": {"email": "prof@uni.example.edu"},
                    "start": {"dateTime": "2026-10-17T10:00:00-05:00"}, "end": {"dateTime": "2026-10-17T11:00:00-05:00"}}]
    real_lc.script(("update_event", {"event_id": event_alias("inv1"), "new_start": "2026-10-17T14:00"}))
    _, r = await review(real_lc, user, "move my advisor meeting to 2")
    assert r["state"] == "read_only_result" and r["screen_lines"] == ["You can't move Advisor meeting",
                                                                      "Someone else organizes it"]


async def test_missing_allowlist_blocks_before_review_too(stub_auth, settings, fake):
    from datetime import datetime
    from zoneinfo import ZoneInfo

    from app.tools import run_tool
    gw = GoogleWorld(stub_auth, settings.model_copy(update={"allowed_recipients": None}),
                     httpx.AsyncClient(transport=httpx.MockTransport(fake.handler)))
    now = datetime(2026, 10, 16, 9, 0, tzinfo=ZoneInfo("America/Chicago"))
    w = await gw.world("u1", now, now.tzinfo)
    assert w.allowed == frozenset()  # not "no limit"
    out = run_tool("draft_email", {"recipient": "Carlos", "subject": "Hi", "body": "Hello."}, w, now)
    assert out.output.state == "read_only_result" and out.output.draft is None
