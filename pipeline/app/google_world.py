"""The connected Google account as a world (B4 step 2): real calendar and contacts, read only.

Same shape as the sample world, so the model, the tools and the safety checks do not change;
only where the data comes from does. Read before the model runs, for every request:
- Calendar: fresh every time (conflict checks need it current), from calendar_past_days back
  to calendar_ahead_days ahead. Past that window the tools say they cannot plan, instead of
  wrongly calling a time free.
- Contacts: saved contacts plus "other contacts" (people the account has emailed), kept
  contacts_cache_s seconds because they rarely change.

The model sees each event under a short alias ("g3f9a1c2") made from a hash of Google's id,
so it never has to copy a 26 character id, and the same event keeps the same alias on every
request ("move that meeting" from memory still finds it). Drafts carry Google's own id for
the executor.

Event titles and notes are untrusted: anyone can put an event on this calendar by sending an
invitation. They reach the model only marked as data (app/tools.py, app/prompt.py).
"""

import asyncio
import hashlib
import time as clock_time
from datetime import date, datetime, time, timedelta
from zoneinfo import ZoneInfo

import httpx

from app.google_auth import GoogleAuth, GoogleError, GoogleNotConnected
from app.mockdata import MockWorld
from app.processing import ScreenError
from app.timing import hop

EVENTS_URL = "https://www.googleapis.com/calendar/v3/calendars/primary/events"
CONNECTIONS_URL = "https://people.googleapis.com/v1/people/me/connections"
OTHER_CONTACTS_URL = "https://people.googleapis.com/v1/otherContacts"
MAX_PAGES = 4
NOTES_CHARS = 300

NOT_CONNECTED = ["Google is not connected", "Connect it in the HaloAI app"]
EXPIRED = ["Google sign in expired", "Connect again in the HaloAI app"]
NO_ANSWER = ["Google did not answer", "Try again in a moment"]
NOT_SET_UP = ["Google is not set up on the server"]


class _Expired(Exception):
    """Google answered 401: the access token is no longer accepted."""


def event_alias(google_id: str, length: int = 8) -> str:
    return "g" + hashlib.sha256(google_id.encode()).hexdigest()[:length - 1]


def contact_id(email: str) -> str:
    return "g_" + hashlib.sha256(email.lower().encode()).hexdigest()[:10]


def map_event(item: dict, tz: ZoneInfo) -> dict | None:
    """One Google event as a world event, or None if it should not appear at all."""
    if item.get("status") == "cancelled" or not item.get("id"):
        return None
    s, e = item.get("start") or {}, item.get("end") or {}
    if "dateTime" in s and "dateTime" in e:
        start = datetime.fromisoformat(s["dateTime"]).astimezone(tz)
        end = datetime.fromisoformat(e["dateTime"]).astimezone(tz)
        all_day = False
    elif "date" in s and "date" in e:  # all day: Google's end date is the day after
        start = datetime.combine(date.fromisoformat(s["date"]), time(0), tz)
        end = datetime.combine(date.fromisoformat(e["date"]), time(0), tz)
        all_day = True
    else:
        return None
    declined = any(a.get("self") and a.get("responseStatus") == "declined" for a in item.get("attendees") or [])
    return {
        "event_id": event_alias(item["id"]), "google_id": item["id"],
        "title": (item.get("summary") or "(no title)").strip()[:80],
        "notes": (item.get("description") or "").strip()[:NOTES_CHARS],
        "start": start, "end": end, "all_day": all_day,
        # Google's own free/busy rule: shown as free, or declined, never blocks a time.
        "busy": item.get("transparency", "opaque") != "transparent" and not declined,
    }


def _people(items: list[dict], key_names: str = "names") -> list[tuple[str, str]]:
    out = []
    for person in items:
        names = person.get(key_names) or []
        name = (names[0].get("displayName") or "").strip() if names else ""
        for addr in person.get("emailAddresses") or []:
            email = (addr.get("value") or "").strip()
            if email:
                out.append((name, email))
    return out


class GoogleWorld:
    def __init__(self, auth: GoogleAuth, settings, http: httpx.AsyncClient | None = None,
                 monotonic=clock_time.monotonic):
        self.auth = auth
        self.settings = settings
        self.http = http or httpx.AsyncClient(timeout=10)
        self.monotonic = monotonic
        self._contacts: dict[str, tuple[float, list[dict]]] = {}  # user_id -> (valid until, contacts)
        auth.on_change.append(self.forget)  # a new or removed account must not keep old contacts

    async def world(self, user_id: str, now: datetime, tz: ZoneInfo) -> MockWorld:
        if not self.auth.configured():
            raise ScreenError(NOT_SET_UP)
        today = now.astimezone(tz).date()
        window = (today - timedelta(days=self.settings.calendar_past_days),
                  today + timedelta(days=self.settings.calendar_ahead_days))
        with hop("google_world") as f:
            try:
                events, (contacts, cached) = await self._with_token(
                    user_id, lambda token: asyncio.gather(self._events(token, window, tz),
                                                          self._contacts_for(user_id, token)))
            except GoogleNotConnected:
                f["result"] = "not_connected"
                raise ScreenError(NOT_CONNECTED) from None
            except (GoogleError, _Expired):
                f["result"] = "expired"
                raise ScreenError(EXPIRED) from None
            except (httpx.HTTPError, ValueError, KeyError):
                f["result"] = "no_answer"
                raise ScreenError(NO_ANSWER) from None
            f.update(events=len(events), contacts=len(contacts), contacts_cached=cached)
        return MockWorld(tz=tz, today=today, contacts=contacts, events=events, source="google", window=window)

    async def _with_token(self, user_id: str, call):
        """Run with a valid access token. One retry with a fresh token if Google refuses the cached one."""
        try:
            return await call(await self.auth.access_token(user_id))
        except _Expired:
            self.auth.drop_access_token(user_id)
            return await call(await self.auth.access_token(user_id))

    async def _get(self, url: str, token: str, params: dict) -> dict:
        resp = await self.http.get(url, headers={"Authorization": f"Bearer {token}"}, params=params)
        if resp.status_code == 401:
            raise _Expired()
        resp.raise_for_status()
        return resp.json()

    async def _pages(self, url: str, token: str, params: dict, items_key: str) -> list[dict]:
        items, page = [], None
        for _ in range(MAX_PAGES):
            data = await self._get(url, token, {**params, **({"pageToken": page} if page else {})})
            items += data.get(items_key) or []
            page = data.get("nextPageToken")
            if not page:
                break
        return items

    async def _events(self, token: str, window: tuple[date, date], tz: ZoneInfo) -> list[dict]:
        items = await self._pages(EVENTS_URL, token, {
            "timeMin": datetime.combine(window[0], time(0), tz).isoformat(),
            "timeMax": datetime.combine(window[1] + timedelta(days=1), time(0), tz).isoformat(),
            "singleEvents": "true",  # a weekly event becomes one entry per week: moving one moves only that one
            "orderBy": "startTime", "maxResults": 250}, "items")
        events, seen = [], set()
        for item in items:
            ev = map_event(item, tz)
            if ev is None:
                continue
            if ev["event_id"] in seen:  # alias collision (rare): a longer alias for the second one
                ev["event_id"] = event_alias(ev["google_id"], 16)
            seen.add(ev["event_id"])
            events.append(ev)
        return events

    async def _contacts_for(self, user_id: str, token: str) -> tuple[list[dict], bool]:
        cached = self._contacts.get(user_id)
        if cached and cached[0] > self.monotonic():
            return [dict(c) for c in cached[1]], True
        saved, other = await asyncio.gather(
            self._pages(CONNECTIONS_URL, token, {"personFields": "names,emailAddresses", "pageSize": 1000},
                        "connections"),
            self._pages(OTHER_CONTACTS_URL, token, {"readMask": "names,emailAddresses", "pageSize": 1000},
                        "otherContacts"))
        contacts, seen = [], set()
        for name, email in _people(saved) + _people(other):  # saved contacts win over other contacts
            if email.lower() in seen:
                continue
            seen.add(email.lower())
            # No name: reachable by its address, never matched by a spoken name.
            contacts.append({"contact_id": contact_id(email), "name": name or email, "email": email})
        self._contacts[user_id] = (self.monotonic() + self.settings.contacts_cache_s, contacts)
        return [dict(c) for c in contacts], False

    def forget(self, user_id: str) -> None:
        """Drop cached contacts, e.g. after the account is disconnected or replaced."""
        self._contacts.pop(user_id, None)
