"""Demo site endpoints (B6, decided 2026-10-19).

The demo site key is inside a public web page, so anyone can copy it. Its only powers: sample
data (mock scope, enforced when keys are created and when executors are chosen), and starting
visitor sessions. Each visitor gets their own sandbox user and a key that expires after
demo_session_hours, so judges never see each other's calendar, emails or memory. Model cost is
capped for all visitors together (demo_daily_requests) and per visitor key (visitor_rate_per_min).
"""

import secrets
import time
from collections import deque
from datetime import timedelta
from uuid import UUID
from zoneinfo import ZoneInfo

from fastapi import APIRouter, Depends, Request
from pydantic import BaseModel

from app.api import json_call
from app.auth import Caller, create_key, require_caller
from app.lifecycle import ApiError
from app.mockdata import DEMO_CONTACTS

router = APIRouter()
VISITOR_CLIENT = "visitor"


class SessionBody(BaseModel):
    call_id: UUID


class SessionLimiter:
    """New visitor sessions per client IP per hour. In memory: correct with one worker."""

    def __init__(self):
        self._seen: dict[str, deque] = {}

    def allow(self, ip: str, per_hour: int) -> bool:
        now = time.monotonic()
        q = self._seen.setdefault(ip, deque())
        while q and q[0] < now - 3600:
            q.popleft()
        if len(q) >= per_hour:
            return False
        q.append(now)
        return True


def _demo_only(caller: Caller) -> None:
    if caller.scope != "mock":
        raise ApiError("invalid_request", 422, "only for the demo site")


async def check_demo_budget(request: Request, caller: Caller) -> None:
    """Every sample mode request that runs the model counts against one shared daily budget: the
    visitors' keys and the demo site's own key, which is public too."""
    if caller.scope != "mock":
        return
    services = request.app.state.services
    since = services.lc.clock.now() - timedelta(hours=24)
    async with services.pool.connection() as conn:
        cur = await conn.execute(
            "SELECT count(*) AS n FROM segments s JOIN requests r ON r.id = s.request_id"
            " JOIN users u ON u.id = r.user_id WHERE u.sandbox AND s.created_at > %s", (since,))
        used = (await cur.fetchone())["n"]
    if used >= services.settings.demo_daily_requests:
        raise ApiError("rate_limited", 429, "the demo is busy today, please try again tomorrow",
                       headers={"Retry-After": "3600"})


@router.post("/v1/demo/session")
async def start_session(body: SessionBody, request: Request, caller: Caller = Depends(require_caller)):
    """A visitor's own sample world. Only the demo site's own key may start one, not a visitor key."""
    services = request.app.state.services

    async def handler():
        _demo_only(caller)
        if caller.client == VISITOR_CLIENT:
            raise ApiError("invalid_request", 422, "a visitor key cannot start another session")
        ip = request.client.host if request.client else "unknown"
        if not services.demo_sessions.allow(ip, services.settings.demo_sessions_per_ip_hour):
            raise ApiError("rate_limited", 429, "too many new sessions from this address, try again later",
                           headers={"Retry-After": "600"})
        expires = services.lc.clock.now() + timedelta(hours=services.settings.demo_session_hours)
        name = "visitor-" + secrets.token_hex(6)
        async with services.pool.connection() as conn:
            cur = await conn.execute("INSERT INTO users (name, sandbox, expires_at) VALUES (%s, true, %s) RETURNING id",
                                     (name, expires))
            user_id = str((await cur.fetchone())["id"])
        token = await create_key(services.pool, user_id, VISITOR_CLIENT, "mock",
                                 rate_per_min=services.settings.visitor_rate_per_min, expires_at=expires)
        return {"token": token, "expires_at": expires.isoformat()}
    return await json_call(request, caller, "demo_session", body, handler)


@router.get("/v1/demo/calendar")
async def calendar(request: Request, tz: str = "America/Chicago", days: int = 7,
                   caller: Caller = Depends(require_caller)):
    """The caller's sample calendar from today, for the page's calendar panel."""
    services = request.app.state.services

    async def handler():
        _demo_only(caller)
        try:
            zone = ZoneInfo(tz)
        except (KeyError, ValueError):
            raise ApiError("invalid_request", 422, "tz must be an IANA name such as America/Chicago") from None
        now = services.lc.clock.now().astimezone(zone)
        world = await services.store.world(caller.user_id, now, zone)  # seeds a new visitor's calendar
        last = now.date() + timedelta(days=max(1, min(days, 31)))
        events = [{"event_id": e["event_id"], "title": e["title"], "start": e["start"].isoformat(),
                   "end": e["end"].isoformat()}
                  for e in world.events if now.date() <= e["start"].date() < last]
        return {"events": events}
    return await json_call(request, caller, "demo_calendar", None, handler)


@router.get("/v1/demo/outbox")
async def outbox(request: Request, caller: Caller = Depends(require_caller)):
    """Emails the visitor approved, sent to the sample outbox (nothing leaves the server)."""
    services = request.app.state.services

    async def handler():
        _demo_only(caller)
        async with services.pool.connection() as conn:
            cur = await conn.execute(
                "SELECT to_addr, subject, body, sent_at FROM mock_outbox WHERE user_id = %s"
                " ORDER BY sent_at DESC LIMIT 20", (caller.user_id,))
            rows = await cur.fetchall()
        return {"emails": [{"to": r["to_addr"], "subject": r["subject"], "body": r["body"],
                            "sent_at": r["sent_at"].isoformat()} for r in rows]}
    return await json_call(request, caller, "demo_outbox", None, handler)


@router.get("/v1/demo/contacts")
async def contacts(request: Request, caller: Caller = Depends(require_caller)):
    """The sample address book, so visitors know who they can email ('email Victor ...')."""
    async def handler():
        _demo_only(caller)
        return {"contacts": [{"name": c["name"], "email": c["email"]} for c in DEMO_CONTACTS]}
    return await json_call(request, caller, "demo_contacts", None, handler)
