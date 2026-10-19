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

# The user's own words asking to save something: "remember that...", and for routines "save a routine
# called gym: ...", "make that my standup routine" (2026-10-19). Naming a routine to use it ("do my gym
# routine") is not a request to save one.
REMEMBER_RE = re.compile(r"\b(remember|don'?t forget|save (that|this)|note that|keep in mind)\b"
                         r"|\b(save|create|make|add|set up|store)\b[^.]{0,40}\broutine\b", re.IGNORECASE)
USUAL_LENGTH = "usual event length"


def remember_intent(segments: list[Segment]) -> bool:
    """True only if the user's own spoken words ask to remember something."""
    return any(s.kind == "voice" and s.text and REMEMBER_RE.search(s.text) for s in segments)


@dataclass
class MemorySnapshot:
    items: list[dict] = field(default_factory=list)    # contact | preference | fact | routine
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


# ---------- view, add, edit and delete (endpoints in app/memory_api.py, BRIEFING section 2) ----------

def _item(row: dict) -> dict:
    return {"memory_id": str(row["id"]), "kind": row["kind"], "label": row["label"], "value": row["value"],
            "source": row["source"], "uses": row["uses"], "updated_at": row["updated_at"].isoformat()}


def _action_item(a: dict) -> dict:
    d = a["draft"]
    kind = d.get("kind")
    if kind == "send_email":
        label, value = f"Emailed {d.get('to_name') or d.get('to')}", f"{d.get('to')}: {d.get('subject')}"
    elif kind == "create_event":
        label, value = f"Created {d.get('title')}", d.get("start", "")
    elif kind == "update_event":
        label, value = f"Moved {d.get('after', {}).get('title')}", d.get("after", {}).get("start", "")
    elif kind == "delete_event":
        label, value = f"Deleted {d.get('event', {}).get('title')}", d.get("event", {}).get("start", "")
    else:
        label, value = str(kind), ""
    return {"memory_id": "act_" + a["key"], "kind": "action", "label": label, "value": value,
            "source": "action", "uses": 0, "updated_at": a["at"].isoformat()}


class MemoryEditor:
    """What the phone app and demo site call. Errors use the contract's codes."""

    def __init__(self, store: MemoryStore):
        self.store = store

    async def list(self, user_id: str) -> dict:
        snap = await self.store.load(user_id, actions=20)
        async with self.store.pool.connection() as conn:
            cur = await conn.execute("SELECT * FROM memory_items WHERE user_id = %s ORDER BY uses DESC, updated_at DESC",
                                     (user_id,))
            items = [_item(r) for r in await cur.fetchall()]
        return {"items": items + [_action_item(a) for a in snap.actions]}

    async def add(self, user_id: str, kind: str, label: str, value: str) -> dict:
        value = _checked_value(kind, value)
        now = self.store.clock.now()
        async with self.store.pool.connection() as conn:
            cur = await conn.execute(
                "INSERT INTO memory_items (user_id, kind, label, value, source, uses, created_at, updated_at)"
                " VALUES (%s, %s, %s, %s, 'user', 0, %s, %s)"
                " ON CONFLICT (user_id, kind, lower(label), lower(value)) DO UPDATE SET updated_at = EXCLUDED.updated_at"
                " RETURNING *", (user_id, kind, label.strip(), value, now, now))
            return _item(await cur.fetchone())

    async def edit(self, user_id: str, memory_id: str, label: str | None, value: str | None) -> dict:
        from app.lifecycle import ApiError
        if memory_id.startswith("act_"):
            raise ApiError("invalid_request", 422, "recent actions can be deleted, not edited")
        async with self.store.pool.connection() as conn:
            cur = await conn.execute("SELECT * FROM memory_items WHERE id::text = %s AND user_id = %s",
                                     (memory_id, user_id))
            row = await cur.fetchone()
            if row is None:
                raise ApiError("not_found", 404, "unknown memory item")
            new_label = (label or row["label"]).strip()
            new_value = _checked_value(row["kind"], value) if value is not None else row["value"]
            try:
                cur = await conn.execute(
                    "UPDATE memory_items SET label = %s, value = %s, source = 'user', updated_at = %s"
                    " WHERE id = %s RETURNING *", (new_label, new_value, self.store.clock.now(), row["id"]))
            except Exception as exc:  # the edit would duplicate another item
                if "memory_items_unique" in str(exc):
                    raise ApiError("invalid_request", 422, "that memory already exists") from None
                raise
            return _item(await cur.fetchone())

    async def delete(self, user_id: str, memory_id: str) -> dict:
        from app.lifecycle import ApiError
        async with self.store.pool.connection() as conn:
            if memory_id.startswith("act_"):  # hide from memory; the execution record stays
                cur = await conn.execute(
                    "UPDATE executions e SET forgotten = true FROM requests r WHERE r.id = e.request_id"
                    " AND r.user_id = %s AND e.execution_key = %s", (user_id, memory_id[4:]))
            else:
                cur = await conn.execute("DELETE FROM memory_items WHERE id::text = %s AND user_id = %s",
                                         (memory_id, user_id))
            if cur.rowcount == 0:
                raise ApiError("not_found", 404, "unknown memory item")
        return {"deleted": True}


def _checked_value(kind: str, value: str) -> str:
    from app.lifecycle import ApiError
    from app.tools import spoken_address
    value = (value or "").strip()
    if not value:
        raise ApiError("invalid_request", 422, "value must not be empty")
    if kind == "contact":
        address = spoken_address(value)
        if not address:
            raise ApiError("invalid_request", 422, "a contact needs an email address")
        return address
    return value
