"""Mock world per user, stored in Postgres (B3): sample data mode and the demo site.

Seeded once on a user's first request, relative to that day, then changed only by
actions the user approved: created, moved and deleted events, and sent emails (into a
mock outbox). So "block the gym" really appears on the calendar, and "move that meeting"
can find an event created a minute ago. Evals keep the frozen in-memory world instead.
"""

from datetime import datetime, time, timedelta
from zoneinfo import ZoneInfo

from psycopg.types.json import Jsonb

from app.mockdata import DEMO_CONTACTS, DEMO_EVENTS, MockWorld
from app.processing import ExecutionOutcome, Executor
from app.tools import fmt_day, fmt_range


def created_event_id(execution_key: str) -> str:
    """Derived from the execution key: a retried create finds its own event instead of adding another."""
    return "ev_" + execution_key[:8]


class MockStore:
    def __init__(self, pool, clock):
        self.pool = pool
        self.clock = clock

    async def world(self, user_id: str, now: datetime, tz: ZoneInfo) -> MockWorld:
        async with self.pool.connection() as conn, conn.transaction():
            cur = await conn.execute(
                "INSERT INTO mock_state (user_id, timezone, seeded_on) VALUES (%s, %s, %s)"
                " ON CONFLICT (user_id) DO NOTHING RETURNING seeded_on",
                (user_id, str(tz), now.astimezone(tz).date()))
            seeded = await cur.fetchone()
            if seeded:  # first use: lay down the sample calendar relative to today
                for event_id, days, start, end, title, notes in DEMO_EVENTS:  # a student's week
                    d = seeded["seeded_on"] + timedelta(days=days)
                    await conn.execute(
                        "INSERT INTO mock_events (user_id, event_id, title, starts_at, ends_at, notes)"
                        " VALUES (%s, %s, %s, %s, %s, %s)",
                        (user_id, event_id, title, datetime.combine(d, time.fromisoformat(start), tz),
                         datetime.combine(d, time.fromisoformat(end), tz), notes))
            cur = await conn.execute("SELECT * FROM mock_events WHERE user_id = %s ORDER BY starts_at", (user_id,))
            rows = await cur.fetchall()
        events = [{"event_id": r["event_id"], "title": r["title"], "notes": r["notes"],
                   "start": r["starts_at"].astimezone(tz), "end": r["ends_at"].astimezone(tz)} for r in rows]
        return MockWorld(tz=tz, today=now.astimezone(tz).date(), contacts=[dict(c) for c in DEMO_CONTACTS], events=events)

    async def apply(self, user_id: str, execution_key: str, draft: dict) -> ExecutionOutcome:
        """Carry out an approved proposal on the user's mock world. Safe to repeat."""
        kind = draft.get("kind")
        async with self.pool.connection() as conn, conn.transaction():
            if kind == "create_event":
                start, end = datetime.fromisoformat(draft["start"]), datetime.fromisoformat(draft["end"])
                event_id = created_event_id(execution_key)
                await conn.execute(
                    "INSERT INTO mock_events (user_id, event_id, title, starts_at, ends_at, guests, video_call, description)"
                    " VALUES (%s, %s, %s, %s, %s, %s, %s, %s) ON CONFLICT (user_id, event_id) DO NOTHING",
                    (user_id, event_id, draft["title"], start, end, Jsonb(draft.get("guests", [])),
                     bool(draft.get("video_call")), draft.get("description", "")))
                return ExecutionOutcome(True, ["Added to your calendar", draft["title"],
                                               f"{fmt_day(start)}, {fmt_range(start, end)}"], {"event_id": event_id})
            if kind == "update_event":
                after = draft["after"]
                start, end = datetime.fromisoformat(after["start"]), datetime.fromisoformat(after["end"])
                cur = await conn.execute(
                    "UPDATE mock_events SET title = %s, starts_at = %s, ends_at = %s WHERE user_id = %s AND event_id = %s",
                    (after["title"], start, end, user_id, draft["event_id"]))
                if cur.rowcount == 0:
                    return ExecutionOutcome(False, ["Not changed", "That event is no longer on your calendar"])
                return ExecutionOutcome(True, ["Event moved", after["title"], f"{fmt_day(start)}, {fmt_range(start, end)}"],
                                        {"event_id": draft["event_id"]})
            if kind == "delete_event":
                await conn.execute("DELETE FROM mock_events WHERE user_id = %s AND event_id = %s",
                                   (user_id, draft["event_id"]))
                # Already gone counts as done: the calendar is in the state the user approved.
                return ExecutionOutcome(True, ["Event deleted", draft["event"]["title"]], {"event_id": draft["event_id"]})
            if kind == "send_email":
                await conn.execute(
                    "INSERT INTO mock_outbox (user_id, execution_key, to_addr, subject, body, sent_at)"
                    " VALUES (%s, %s, %s, %s, %s, %s) ON CONFLICT (execution_key) DO NOTHING",
                    (user_id, execution_key, draft["to"], draft["subject"], draft["body"], self.clock.now()))
                return ExecutionOutcome(True, ["Email sent (sample data)", f"To: {draft['to']}", f"Re: {draft['subject']}"])
        return ExecutionOutcome(False, ["Not done", f"Unknown action {kind}"])


class MockExecutor(Executor):
    """Applies approved actions to the user's mock world. Never touches anything real."""

    name = "mock"

    def __init__(self, store: MockStore):
        self.store = store

    async def execute(self, draft: dict, execution_key: str, user_id: str = "") -> ExecutionOutcome:
        if not user_id:
            raise ValueError("the mock executor needs the user")
        return await self.store.apply(user_id, execution_key, draft)
