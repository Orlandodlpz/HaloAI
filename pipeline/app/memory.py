"""Memory (B3): contacts, preferences and facts, plus recent actions.

Two ways in, and only two (decided 2026-10-15):
- learned from actions the user approved (who they email or invite, how often, their usual
  event length): the user saw each address on REVIEW and pressed approve;
- "remember that..." in the user's own words, checked in code (see remember_intent).
Never from content: web pages, email text and event notes cannot write memory, so an
injected "remember that Victor's email is attacker@..." has nowhere to land.
"""

import re
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime

from app.processing import Segment

REMEMBER_RE = re.compile(r"\b(remember|don'?t forget|save (that|this)|note that|keep in mind)\b", re.IGNORECASE)
USUAL_LENGTH = "usual event length"


def remember_intent(segments: list[Segment]) -> bool:
    """True only if the user's own spoken words ask to remember something."""
    return any(s.kind == "voice" and s.text and REMEMBER_RE.search(s.text) for s in segments)


@dataclass
class MemorySnapshot:
    items: list[dict] = field(default_factory=list)    # contact | preference | fact
    actions: list[dict] = field(default_factory=list)  # recent approved actions, newest first


class MemoryStore:
    def __init__(self, pool, clock):
        self.pool = pool
        self.clock = clock

    async def load(self, user_id: str, actions: int = 5) -> MemorySnapshot:
        async with self.pool.connection() as conn:
            cur = await conn.execute(
                "SELECT id, kind, label, value, source, uses, updated_at FROM memory_items WHERE user_id = %s"
                " ORDER BY uses DESC, updated_at DESC", (user_id,))
            items = [{**r, "id": str(r["id"])} for r in await cur.fetchall()]
            cur = await conn.execute(
                "SELECT e.execution_key, e.updated_at, e.detail, v.draft FROM executions e"
                " JOIN requests r ON r.id = e.request_id"
                " JOIN revisions v ON v.request_id = e.request_id AND v.revision = e.revision"
                " WHERE r.user_id = %s AND e.status = 'applied' AND NOT e.forgotten"
                " ORDER BY e.updated_at DESC LIMIT %s", (user_id, actions))
            acts = [{"key": r["execution_key"], "at": r["updated_at"], "draft": r["draft"] or {},
                     "detail": r["detail"] or {}} for r in await cur.fetchall()]
        return MemorySnapshot(items, acts)

    async def _upsert(self, conn, user_id: str, kind: str, label: str, value: str, source: str, uses: int) -> None:
        now = self.clock.now()
        await conn.execute(
            "INSERT INTO memory_items (user_id, kind, label, value, source, uses, created_at, updated_at)"
            " VALUES (%s, %s, %s, %s, %s, %s, %s, %s)"
            " ON CONFLICT (user_id, kind, lower(label), lower(value))"
            " DO UPDATE SET uses = memory_items.uses + EXCLUDED.uses, updated_at = EXCLUDED.updated_at",
            (user_id, kind, label, value, source, uses, now, now))

    async def apply_writes(self, conn, user_id: str, writes: list[dict]) -> None:
        """'Remember that...' saves, applied in the same transaction that stores the result."""
        for w in writes:
            await self._upsert(conn, user_id, w["kind"], w["label"], w["value"], "user", 0)

    async def learn_from_execution(self, conn, user_id: str, draft: dict) -> None:
        """Learn from an action the user approved and that was applied. Never from content."""
        kind = draft.get("kind")
        if kind == "send_email" and draft.get("to"):
            label = draft.get("to_name") or draft["to"]
            await self._upsert(conn, user_id, "contact", label, draft["to"], "learned", 1)
        if kind == "create_event":
            for guest in draft.get("guests", []):
                await self._upsert(conn, user_id, "contact", guest["name"], guest["email"], "learned", 1)
            await self._learn_usual_length(conn, user_id)

    async def _learn_usual_length(self, conn, user_id: str) -> None:
        """If the last approved events mostly share a length other than one hour, remember it."""
        cur = await conn.execute(
            "SELECT v.draft FROM executions e JOIN requests r ON r.id = e.request_id"
            " JOIN revisions v ON v.request_id = e.request_id AND v.revision = e.revision"
            " WHERE r.user_id = %s AND e.status = 'applied' AND v.draft->>'kind' = 'create_event'"
            " ORDER BY e.updated_at DESC LIMIT 10", (user_id,))
        minutes = [round((datetime.fromisoformat(r["draft"]["end"]) - datetime.fromisoformat(r["draft"]["start"]))
                         .total_seconds() / 60) for r in await cur.fetchall()]
        if len(minutes) < 3:
            return
        length, count = Counter(minutes).most_common(1)[0]
        if count * 2 <= len(minutes):
            return  # no clear habit
        await conn.execute("DELETE FROM memory_items WHERE user_id = %s AND kind = 'preference'"
                           " AND source = 'learned' AND lower(label) = %s", (user_id, USUAL_LENGTH))
        if length != 60:
            await self._upsert(conn, user_id, "preference", USUAL_LENGTH, f"{length} minutes", "learned", 0)
