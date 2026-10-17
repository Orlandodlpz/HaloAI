"""Carries out an approved proposal in the connected Google account (B4 step 3).

The only code that changes anything in Google, and it runs only after a single press in REVIEW
(the lifecycle calls it once per execution key). Gmail is contacted here and nowhere else.

Outcomes, which decide what the device may do with its saved key:
- applied: done (SUCCESS).
- not applied: Google refused before acting, or our own checks did (FAILURE, nothing happened).
- an exception: we cannot know whether it happened (EXECUTION UNCERTAIN). Network errors,
  timeouts and Google 5xx land here, never as "sent".

Recipients: tools.py already refused any address the user did not say or have as a contact. Here,
email and invitations to addresses outside the team list (ALLOWED_RECIPIENTS) count against a daily
cap, so a leaked phone key cannot turn the demo account into a spam source (decided 2026-10-16).

Duplicates: a created event's id comes from the execution key, so if a create is ever repeated
Google answers "already exists" instead of making a second event. Gmail has no such id and we
have no read access, so a send that times out stays uncertain.
"""

import base64
import hashlib
from datetime import datetime, timedelta
from email.message import EmailMessage

import httpx

from app.google_auth import GoogleAuth, GoogleError
from app.google_world import EXPIRED, allowed_recipients, event_alias
from app.processing import ExecutionOutcome, Executor
from app.tools import EMAIL_RE, fmt_day, fmt_range

EVENTS_URL = "https://www.googleapis.com/calendar/v3/calendars/primary/events"
SEND_URL = "https://gmail.googleapis.com/gmail/v1/users/me/messages/send"


class _Expired(Exception):
    """Google answered 401: the request was refused before anything happened."""


def created_google_id(execution_key: str) -> str:
    """Google event ids allow a to v and 0 to 9; hex digits fit. Same key, same id."""
    return "halo" + hashlib.sha256(execution_key.encode()).hexdigest()[:26]


def _one_line(text: str) -> str:
    return " ".join(str(text).split())  # no line breaks in a mail header


def recipients(draft: dict) -> list[str]:
    """Who an approved draft sends to: the email's address, or the new event's guests."""
    if draft.get("kind") == "send_email":
        return [draft.get("to", "")]
    if draft.get("kind") == "create_event":
        return [g["email"] for g in draft.get("guests", [])]
    return []


class GoogleExecutor(Executor):
    name = "google"

    def __init__(self, auth: GoogleAuth, settings, pool, clock, http: httpx.AsyncClient | None = None):
        self.auth = auth
        self.settings = settings
        self.pool = pool
        self.clock = clock
        self.http = http or httpx.AsyncClient(timeout=10)

    async def execute(self, draft: dict, execution_key: str, user_id: str = "") -> ExecutionOutcome:
        if draft.get("source") != "google" or not user_id:
            return ExecutionOutcome(False, ["Not done", "This proposal was not made from your Google account"])
        kind = draft.get("kind")
        handler = {"create_event": self._create, "update_event": self._update,
                   "delete_event": self._delete, "send_email": self._send}.get(kind)
        if handler is None:
            return ExecutionOutcome(False, ["Not done", f"Unknown action {kind}"])
        team = allowed_recipients(self.settings)
        outside = [r for r in recipients(draft) if r.lower() not in team]
        if outside and await self._outside_today(user_id, execution_key, team) + len(outside) \
                > self.settings.outside_recipients_per_day:
            return ExecutionOutcome(False, ["Not done", "Daily limit for new addresses reached",
                                            "Team addresses still work"])
        try:
            try:
                return await handler(draft, execution_key, await self.auth.access_token(user_id))
            except _Expired:  # refused before acting: safe to retry once with a fresh token
                self.auth.drop_access_token(user_id)
                return await handler(draft, execution_key, await self.auth.access_token(user_id))
        except (GoogleError, _Expired):
            return ExecutionOutcome(False, ["Not done", *EXPIRED])

    async def _outside_today(self, user_id: str, execution_key: str, team: frozenset) -> int:
        """Recipients outside the team list in this user's real sends of the last 24 hours, counting
        sends that may have happened (pending, unknown) as well as done ones."""
        since = self.clock.now() - timedelta(hours=24)
        async with self.pool.connection() as conn:
            cur = await conn.execute(
                "SELECT v.draft FROM executions e"
                " JOIN revisions v ON v.request_id = e.request_id AND v.revision = e.revision"
                " JOIN requests r ON r.id = e.request_id"
                " WHERE r.user_id = %s AND e.created_at > %s AND e.execution_key <> %s"
                " AND e.status IN ('pending', 'applied', 'unknown')", (user_id, since, execution_key))
            drafts = [row["draft"] for row in await cur.fetchall()]
        return sum(1 for d in drafts if d and d.get("source") == "google"
                   for r in recipients(d) if r.lower() not in team)

    # ---------- HTTP ----------

    async def _call(self, method: str, url: str, token: str, **kwargs) -> httpx.Response:
        headers = {"Authorization": f"Bearer {token}", **kwargs.pop("headers", {})}
        resp = await self.http.request(method, url, headers=headers, **kwargs)
        if resp.status_code == 401:
            raise _Expired()
        if resp.status_code >= 500:
            resp.raise_for_status()  # Google may or may not have acted: uncertain, never "done"
        return resp

    # ---------- calendar ----------

    async def _create(self, draft: dict, key: str, token: str) -> ExecutionOutcome:
        guests = [g["email"] for g in draft.get("guests", [])]
        start, end = datetime.fromisoformat(draft["start"]), datetime.fromisoformat(draft["end"])
        google_id = created_google_id(key)
        body = {"id": google_id, "summary": draft["title"],
                "start": {"dateTime": draft["start"], "timeZone": draft["timezone"]},
                "end": {"dateTime": draft["end"], "timeZone": draft["timezone"]}}
        if draft.get("description"):
            body["description"] = draft["description"]
        if guests:
            body["attendees"] = [{"email": g} for g in guests]
        if draft.get("video_call"):  # the request id makes a repeated create reuse the same Meet link
            body["conferenceData"] = {"createRequest": {"requestId": key,
                                                        "conferenceSolutionKey": {"type": "hangoutsMeet"}}}
        params = {"conferenceDataVersion": 1, "sendUpdates": "all" if guests else "none"}
        resp = await self._call("POST", EVENTS_URL, token, params=params, json=body)
        if resp.status_code == 409:  # this key already created it
            resp = await self._call("GET", f"{EVENTS_URL}/{google_id}", token)
            if resp.status_code != 200 or resp.json().get("status") == "cancelled":
                return ExecutionOutcome(False, ["Not added", "It was created before and then deleted"])
        elif resp.status_code != 200:
            return ExecutionOutcome(False, ["Not added", f"Google refused it ({resp.status_code})"])
        event = resp.json()
        lines = ["Added to your calendar", draft["title"], f"{fmt_day(start)}, {fmt_range(start, end)}"]
        if guests:
            lines.append("Invitations sent")
        if event.get("hangoutLink"):
            lines.append("Google Meet link added")
        return ExecutionOutcome(True, lines, {"event_id": event_alias(google_id), "google_id": google_id})

    def _notify(self, draft: dict) -> str:
        """Guests hear about a move or delete: only the organizer can do either, so they are people
        the user invited."""
        return "all" if draft.get("guest_emails") else "none"

    async def _update(self, draft: dict, key: str, token: str) -> ExecutionOutcome:
        after = draft["after"]
        start, end = datetime.fromisoformat(after["start"]), datetime.fromisoformat(after["end"])
        body = {"summary": after["title"], "start": {"dateTime": after["start"], "timeZone": draft["timezone"]},
                "end": {"dateTime": after["end"], "timeZone": draft["timezone"]}}
        resp = await self._call("PATCH", f"{EVENTS_URL}/{draft['google_id']}", token, json=body,
                                params={"sendUpdates": self._notify(draft)},
                                headers={"If-Match": draft.get("etag") or "*"})
        if resp.status_code == 412:
            return ExecutionOutcome(False, ["Not changed", "That event changed since you saw it"])
        if resp.status_code in (404, 410):
            return ExecutionOutcome(False, ["Not changed", "That event is no longer on your calendar"])
        if resp.status_code != 200:
            return ExecutionOutcome(False, ["Not changed", f"Google refused it ({resp.status_code})"])
        return ExecutionOutcome(True, ["Event moved", after["title"], f"{fmt_day(start)}, {fmt_range(start, end)}"],
                                {"event_id": draft["event_id"]})

    async def _delete(self, draft: dict, key: str, token: str) -> ExecutionOutcome:
        resp = await self._call("DELETE", f"{EVENTS_URL}/{draft['google_id']}", token,
                                params={"sendUpdates": self._notify(draft)},
                                headers={"If-Match": draft.get("etag") or "*"})
        if resp.status_code == 412:
            return ExecutionOutcome(False, ["Not deleted", "That event changed since you saw it"])
        if resp.status_code not in (200, 204, 404, 410):  # already gone counts as done
            return ExecutionOutcome(False, ["Not deleted", f"Google refused it ({resp.status_code})"])
        return ExecutionOutcome(True, ["Event deleted", draft["event"]["title"]], {"event_id": draft["event_id"]})

    # ---------- email ----------

    async def _send(self, draft: dict, key: str, token: str) -> ExecutionOutcome:
        to = str(draft.get("to", "")).strip()
        if not EMAIL_RE.fullmatch(to.lower()):
            return ExecutionOutcome(False, ["Not sent", "The address is not valid"])
        msg = EmailMessage()
        msg["To"] = to
        msg["Subject"] = _one_line(draft["subject"])
        msg["Message-ID"] = f"<{key}@haloai-cov.io>"  # traceable to this approval in the recipient's mail
        msg.set_content(draft["body"])
        raw = base64.urlsafe_b64encode(msg.as_bytes()).decode()
        resp = await self._call("POST", SEND_URL, token, json={"raw": raw})
        if resp.status_code != 200:  # 4xx: Gmail refused it, nothing was sent
            return ExecutionOutcome(False, ["Not sent", f"Gmail refused it ({resp.status_code})"])
        return ExecutionOutcome(True, ["Email sent", f"To: {to}", f"Re: {msg['Subject']}"])
