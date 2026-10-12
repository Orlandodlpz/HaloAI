"""Request lifecycle: revisions, decisions, approval exactly once, expiry.

Every change for a user runs in one transaction that first locks that user's row
(SELECT ... FOR UPDATE). Two calls for the same user therefore run one after the
other, never interleaved: that is what makes "approve twice, execute once" and
"one request at a time" hold under concurrent requests.

Slow work (the model, Gmail, Calendar) runs outside transactions. Its result is
written only if the request is still on the same revision when it finishes;
otherwise it is dropped (latest revision wins).
"""

import asyncio
from collections.abc import Awaitable, Callable
from datetime import timedelta

from psycopg.types.json import Jsonb
from psycopg_pool import AsyncConnectionPool

from app import ids
from app.config import Settings
from app.processing import Executor, ProcessContext, Processor, ProcessorOutput, Segment
from app.screens import OUTCOME_LINES, wrap

OPEN_STATES = ("processing", "conflict_ambiguous", "conflict_schedule", "review", "executing")
DECISION_STATES = ("conflict_ambiguous", "conflict_schedule", "review")
EXECUTED_STATES = ("executing", "success", "failure", "execution_uncertain")
PRE_APPROVAL_STATES = ("processing", "conflict_ambiguous", "conflict_schedule", "review", "read_only_result")
EXECUTION_STATE = {"pending": "executing", "applied": "success", "not_applied": "failure", "unknown": "execution_uncertain"}


class ApiError(Exception):
    """An error with the contract's code; current is the request's result, if any."""

    def __init__(self, code: str, http_status: int, message: str = "", current: dict | None = None):
        super().__init__(message or code)
        self.code = code
        self.http_status = http_status
        self.message = message or code
        self.current = current


async def _no_status(_line: str) -> None:
    return None


class Lifecycle:
    def __init__(self, pool: AsyncConnectionPool, settings: Settings, clock, processor: Processor,
                 executors: dict[str, Executor]):
        self.pool = pool
        self.settings = settings
        self.clock = clock
        self.processor = processor
        self.executors = executors  # {'mock': ..., 'real': ...}
        self._tasks: set[asyncio.Task] = set()

    # ---------- helpers ----------

    def executor_for(self, scope: str) -> Executor:
        """Demo keys ('mock') never reach real tools; real tools stay off until B4."""
        if scope == "real" and self.settings.real_tools_enabled:
            return self.executors["real"]
        return self.executors["mock"]

    async def ensure_user(self, name: str) -> str:
        async with self.pool.connection() as conn:
            cur = await conn.execute(
                "INSERT INTO users (name) VALUES (%s) ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.name"
                " RETURNING id", (name,))
            return str((await cur.fetchone())["id"])

    async def _lock_user(self, conn, user_id: str) -> None:
        await conn.execute("SELECT id FROM users WHERE id = %s FOR UPDATE", (user_id,))

    async def _load(self, conn, user_id: str, request_id: str) -> dict:
        cur = await conn.execute("SELECT * FROM requests WHERE id = %s AND user_id = %s", (request_id, user_id))
        req = await cur.fetchone()
        if req is None:
            raise ApiError("not_found", 404, "unknown request")
        return req

    async def _history(self, conn, user_id: str, request_id: str | None, event: str, data: dict | None = None):
        await conn.execute(
            "INSERT INTO history (user_id, request_id, event, data, at) VALUES (%s, %s, %s, %s, %s)",
            (user_id, request_id, event, Jsonb(data) if data is not None else None, self.clock.now()))

    async def _set_state(self, conn, req: dict, **fields) -> dict:
        fields["updated_at"] = self.clock.now()
        cols = ", ".join(f"{k} = %s" for k in fields)
        cur = await conn.execute(f"UPDATE requests SET {cols} WHERE id = %s RETURNING *",
                                 (*fields.values(), req["id"]))
        return await cur.fetchone()

    async def _expire_if_due(self, conn, req: dict) -> dict:
        """Expiry is checked whenever a request is touched: an expired proposal can never be approved."""
        now = self.clock.now()
        if req["state"] in DECISION_STATES and req["expires_at"] is not None and req["expires_at"] <= now:
            req = await self._set_state(conn, req, state="cancelled", cancel_reason="expired", expires_at=None)
            await self._history(conn, req["user_id"], req["id"], "expired")
        return req

    async def _closed_check(self, conn, req: dict) -> dict:
        """Raise if the request can no longer take user input before approval."""
        req = await self._expire_if_due(conn, req)
        if req["state"] in EXECUTED_STATES:
            raise ApiError("already_executing", 409, "request already approved", await self._result(conn, req))
        if req["state"] in ("cancelled", "read_only_result"):
            raise ApiError("expired", 409, f"request is {req['state']}", await self._result(conn, req))
        return req

    def _new_expiry(self):
        return self.clock.now() + timedelta(seconds=self.settings.expiry_s)

    async def _result(self, conn, req: dict) -> dict:
        cur = await conn.execute("SELECT * FROM revisions WHERE request_id = %s AND revision = %s",
                                 (req["id"], req["current_revision"]))
        rev = await cur.fetchone()
        cur = await conn.execute("SELECT * FROM executions WHERE request_id = %s", (req["id"],))
        exe = await cur.fetchone()
        return self.build_result(req, req["state"], req["current_revision"], rev, exe)

    def build_result(self, req: dict, state: str, revision: int, rev: dict | None, exe: dict | None) -> dict:
        """The one result shape from BRIEFING section 2."""
        now = self.clock.now()
        if state in ("conflict_ambiguous", "conflict_schedule", "read_only_result") and rev:
            lines = rev["screen_lines"]
        elif state == "review" and rev:
            lines = rev["review_screen_lines"] or rev["screen_lines"]
        elif state in EXECUTED_STATES and exe and exe["screen_lines"]:
            lines = exe["screen_lines"]
        elif state == "failure" and rev and rev["state"] == "failure":
            lines = rev["screen_lines"]
        else:
            lines = OUTCOME_LINES.get(state, [state])
        expires_at = req.get("expires_at") if state in DECISION_STATES else None
        show_proposal = state == "review" or state in EXECUTED_STATES
        return {
            "state": state,
            "request_id": str(req["id"]),
            "revision": revision,
            "screen_lines": wrap(lines, self.settings, req["orientation"]),
            "candidates": (rev["candidates"] if rev else None) if state == "conflict_ambiguous" else None,
            "proposal_hash": rev["proposal_hash"] if rev and show_proposal else None,
            "execution_key": exe["execution_key"] if exe else None,
            "draft": rev["draft"] if rev and state not in ("processing", "cancelled") else None,
            "expires_at": expires_at.isoformat() if expires_at else None,
            "expires_in_s": max(0, round((expires_at - now).total_seconds())) if expires_at else None,
            "timings": dict(rev["timings"]) if rev and rev["timings"] else {},
        }

    async def get_result(self, user_id: str, request_id: str) -> dict:
        async with self.pool.connection() as conn:
            return await self._result(conn, await self._load(conn, user_id, request_id))

    # ---------- inputs that create revisions ----------

    async def start(self, user_id: str, scope: str, body: dict) -> int:
        """New request at revision 1. Cancels an older unapproved request; refused while
        an older request is executing."""
        now = self.clock.now()
        async with self.pool.connection() as conn, conn.transaction():
            await self._lock_user(conn, user_id)
            cur = await conn.execute("SELECT id FROM requests WHERE id = %s", (body["request_id"],))
            if await cur.fetchone():
                raise ApiError("invalid_request", 422, "request_id already used")
            cur = await conn.execute(
                "SELECT * FROM requests WHERE user_id = %s AND state = ANY(%s)", (user_id, list(OPEN_STATES)))
            old = await cur.fetchone()
            if old is not None:
                if old["state"] == "executing":
                    raise ApiError("busy", 409, "a previous request is executing", await self._result(conn, old))
                await self._set_state(conn, old, state="cancelled", cancel_reason="replaced", expires_at=None)
                await self._history(conn, user_id, str(old["id"]), "cancelled", {"reason": "replaced"})
            await conn.execute(
                "INSERT INTO requests (id, user_id, scope, state, current_revision, timezone, language,"
                " orientation, created_at, updated_at) VALUES (%s, %s, %s, 'processing', 1, %s, %s, %s, %s, %s)",
                (body["request_id"], user_id, scope, body["timezone"], body["language"],
                 body["orientation"], now, now))
            await conn.execute(
                "INSERT INTO segments (request_id, revision, kind, segment_id, text, sent_at, created_at)"
                " VALUES (%s, 1, 'voice', %s, %s, %s, %s)",
                (body["request_id"], body["segment_id"], body["text"], body["sent_at"], now))
            await self._history(conn, user_id, body["request_id"], "start", {"text": body["text"]})
        return 1

    async def add(self, user_id: str, request_id: str, body: dict) -> int:
        """Addition: appended to the same request, never rejected for a stale base revision."""
        now = self.clock.now()
        async with self.pool.connection() as conn, conn.transaction():
            await self._lock_user(conn, user_id)
            req = await self._closed_check(conn, await self._load(conn, user_id, request_id))
            new_rev = req["current_revision"] + 1
            await conn.execute(
                "INSERT INTO segments (request_id, revision, kind, segment_id, text, sent_at, created_at)"
                " VALUES (%s, %s, 'voice', %s, %s, %s, %s)",
                (request_id, new_rev, body["segment_id"], body["text"], body["sent_at"], now))
            await self._set_state(conn, req, state="processing", current_revision=new_rev, expires_at=None)
            await self._history(conn, user_id, request_id, "addition",
                                {"base": body.get("revision"), "revision": new_rev, "text": body["text"]})
        return new_rev

    async def select(self, user_id: str, request_id: str, body: dict) -> int:
        now = self.clock.now()
        async with self.pool.connection() as conn, conn.transaction():
            await self._lock_user(conn, user_id)
            req = await self._closed_check(conn, await self._load(conn, user_id, request_id))
            if req["state"] != "conflict_ambiguous" or body["revision"] != req["current_revision"]:
                raise ApiError("stale_revision", 409, "not the displayed revision", await self._result(conn, req))
            cur = await conn.execute("SELECT candidates FROM revisions WHERE request_id = %s AND revision = %s",
                                     (request_id, req["current_revision"]))
            candidates = (await cur.fetchone())["candidates"] or []
            if body["candidate_id"] not in {c["candidate_id"] for c in candidates}:
                raise ApiError("invalid_request", 422, "unknown candidate_id")
            new_rev = req["current_revision"] + 1
            await conn.execute(
                "INSERT INTO segments (request_id, revision, kind, candidate_id, created_at)"
                " VALUES (%s, %s, 'select', %s, %s)", (request_id, new_rev, body["candidate_id"], now))
            await self._set_state(conn, req, state="processing", current_revision=new_rev, expires_at=None)
            await self._history(conn, user_id, request_id, "select",
                                {"candidate_id": body["candidate_id"], "revision": new_rev})
        return new_rev

    async def process(self, request_id: str, revision: int,
                      on_status: Callable[[str], Awaitable[None]] = _no_status) -> dict:
        """Run the processor for one revision. The result is stored only if the request is
        still on that revision; a cancelled request returns its cancelled result."""
        async with self.pool.connection() as conn:
            cur = await conn.execute("SELECT * FROM requests WHERE id = %s", (request_id,))
            req = await cur.fetchone()
            cur = await conn.execute(
                "SELECT * FROM segments WHERE request_id = %s AND revision <= %s ORDER BY revision",
                (request_id, revision))
            segs = [Segment(s["revision"], s["kind"], s["text"], s["candidate_id"], s["sent_at"])
                    for s in await cur.fetchall()]
        ctx = ProcessContext(str(request_id), revision, segs, req["timezone"], req["language"],
                             req["orientation"], req["scope"], on_status)
        failure_reason = None
        try:
            output = await asyncio.wait_for(self.processor.process(ctx), self.settings.processing_deadline_s)
        except Exception as exc:  # model error or deadline: FAILURE, never a guess
            failure_reason = type(exc).__name__
            output = ProcessorOutput("failure", OUTCOME_LINES["failure"])

        proposal = ids.proposal_hash(output.draft) if output.draft is not None else None
        rev_row = {
            "state": output.state, "screen_lines": output.screen_lines,
            "review_screen_lines": output.review_screen_lines, "candidates": output.candidates,
            "draft": output.draft, "proposal_hash": proposal, "timings": output.timings,
        }
        async with self.pool.connection() as conn, conn.transaction():
            await self._lock_user(conn, req["user_id"])
            cur = await conn.execute("SELECT * FROM requests WHERE id = %s", (request_id,))
            req = await cur.fetchone()
            if req["state"] == "cancelled":
                return await self._result(conn, req)
            if req["current_revision"] != revision or req["state"] != "processing":
                # A newer revision exists: this result is stale. Returned for the old
                # stream (the phone drops it by revision) but never stored or shown.
                return self.build_result(req, output.state, revision, rev_row, None)
            await conn.execute(
                "INSERT INTO revisions (request_id, revision, state, screen_lines, review_screen_lines,"
                " candidates, draft, proposal_hash, timings, created_at)"
                " VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
                (request_id, revision, output.state, Jsonb(output.screen_lines),
                 Jsonb(output.review_screen_lines) if output.review_screen_lines is not None else None,
                 Jsonb(output.candidates) if output.candidates is not None else None,
                 Jsonb(output.draft) if output.draft is not None else None,
                 proposal, Jsonb(output.timings), self.clock.now()))
            expires = self._new_expiry() if output.state in DECISION_STATES else None
            req = await self._set_state(conn, req, state=output.state, expires_at=expires,
                                        failure_reason=failure_reason)
            await self._history(conn, req["user_id"], str(request_id), "result",
                                {"revision": revision, "state": output.state, "error": failure_reason})
            return await self._result(conn, req)

    # ---------- decisions ----------

    async def continue_(self, user_id: str, request_id: str, body: dict) -> dict:
        async with self.pool.connection() as conn, conn.transaction():
            await self._lock_user(conn, user_id)
            req = await self._load(conn, user_id, request_id)
            if req["state"] == "review" and body["revision"] == req["current_revision"]:
                return await self._result(conn, req)  # already continued: harmless repeat
            req = await self._closed_check(conn, req)
            if req["state"] != "conflict_schedule" or body["revision"] != req["current_revision"]:
                raise ApiError("stale_revision", 409, "not the displayed revision", await self._result(conn, req))
            req = await self._set_state(conn, req, state="review", expires_at=self._new_expiry())
            await self._history(conn, user_id, request_id, "continue", {"revision": body["revision"]})
            return await self._result(conn, req)

    async def approve(self, user_id: str, request_id: str, body: dict) -> dict:
        """Approval exactly once. The execution key is derived from request, revision and
        proposal hash, so any repeat of the same approval finds the same execution."""
        key = ids.execution_key(str(request_id), body["revision"], body["proposal_hash"])
        async with self.pool.connection() as conn, conn.transaction():
            await self._lock_user(conn, user_id)
            req = await self._load(conn, user_id, request_id)
            cur = await conn.execute("SELECT * FROM executions WHERE execution_key = %s", (key,))
            if await cur.fetchone():
                return await self._result(conn, req)  # same approval again: no second execution
            req = await self._closed_check(conn, req)
            if req["state"] != "review" or body["revision"] != req["current_revision"]:
                raise ApiError("stale_revision", 409, "not the displayed revision", await self._result(conn, req))
            cur = await conn.execute("SELECT draft, proposal_hash FROM revisions WHERE request_id = %s AND revision = %s",
                                     (request_id, req["current_revision"]))
            rev = await cur.fetchone()
            if body["proposal_hash"] != rev["proposal_hash"]:
                raise ApiError("hash_mismatch", 409, "proposal changed", await self._result(conn, req))
            now = self.clock.now()
            await conn.execute(
                "INSERT INTO executions (execution_key, request_id, revision, proposal_hash, status,"
                " created_at, updated_at) VALUES (%s, %s, %s, %s, 'pending', %s, %s)",
                (key, request_id, body["revision"], body["proposal_hash"], now, now))
            await self._set_state(conn, req, state="executing", expires_at=None)
            await self._history(conn, user_id, request_id, "approve", {"revision": body["revision"], "key": key})
            draft, scope = rev["draft"], req["scope"]

        # Outside the transaction: run the action in the background and wait a bounded time.
        # If the wait runs out, the phone gets 'executing' and polls the status endpoint;
        # the action itself keeps running and records its outcome when done.
        task = asyncio.create_task(self._run_execution(user_id, request_id, key, draft, self.executor_for(scope)))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
        await asyncio.wait({task}, timeout=self.settings.approve_wait_s)
        return await self.get_result(user_id, request_id)

    async def _run_execution(self, user_id: str, request_id: str, key: str, draft: dict, executor: Executor):
        screen_lines, detail = None, {"executor": executor.name}
        try:
            outcome = await asyncio.wait_for(executor.execute(draft, key), self.settings.execution_deadline_s)
            status, screen_lines = ("applied" if outcome.applied else "not_applied"), outcome.screen_lines
            detail.update(outcome.detail)
        except Exception as exc:  # timeout or error: we cannot know if it happened
            status = "unknown"
            detail["error"] = type(exc).__name__
        async with self.pool.connection() as conn, conn.transaction():
            await self._lock_user(conn, user_id)
            await conn.execute(
                "UPDATE executions SET status = %s, screen_lines = %s, detail = %s, updated_at = %s"
                " WHERE execution_key = %s",
                (status, Jsonb(screen_lines) if screen_lines else None, Jsonb(detail), self.clock.now(), key))
            req = await self._load(conn, user_id, request_id)
            await self._set_state(conn, req, state=EXECUTION_STATE[status])
            await self._history(conn, user_id, request_id, "execution", {"key": key, "status": status})

    async def cancel(self, user_id: str, request_id: str) -> dict:
        async with self.pool.connection() as conn, conn.transaction():
            await self._lock_user(conn, user_id)
            req = await self._expire_if_due(conn, await self._load(conn, user_id, request_id))
            if req["state"] in EXECUTED_STATES:
                raise ApiError("already_executing", 409, "approved actions cannot be cancelled",
                               await self._result(conn, req))
            if req["state"] in PRE_APPROVAL_STATES:
                req = await self._set_state(conn, req, state="cancelled", cancel_reason="user", expires_at=None)
                await self._history(conn, user_id, request_id, "cancelled", {"reason": "user"})
            return await self._result(conn, req)  # already cancelled or expired: harmless repeat

    async def touch(self, user_id: str, request_id: str, revision: int) -> dict:
        """Knob activity: resets the 60 s expiry of the displayed decision state."""
        async with self.pool.connection() as conn, conn.transaction():
            await self._lock_user(conn, user_id)
            req = await self._closed_check(conn, await self._load(conn, user_id, request_id))
            if req["state"] not in DECISION_STATES or revision != req["current_revision"]:
                raise ApiError("stale_revision", 409, "not the displayed revision", await self._result(conn, req))
            req = await self._set_state(conn, req, expires_at=self._new_expiry())
            return await self._result(conn, req)

    async def execution_status(self, user_id: str, key: str) -> dict:
        async with self.pool.connection() as conn:
            cur = await conn.execute(
                "SELECT r.* FROM executions e JOIN requests r ON r.id = e.request_id"
                " WHERE e.execution_key = %s AND r.user_id = %s", (key, user_id))
            req = await cur.fetchone()
            if req is None:
                raise ApiError("not_found", 404, "unknown execution key")
            return await self._result(conn, req)

    # ---------- startup and tests ----------

    async def recover_on_startup(self) -> None:
        """After a restart, work that was in flight is gone: an execution in flight becomes
        unknown (it may have happened), processing becomes failure."""
        now = self.clock.now()
        async with self.pool.connection() as conn, conn.transaction():
            await conn.execute("UPDATE executions SET status = 'unknown', updated_at = %s WHERE status = 'pending'", (now,))
            await conn.execute("UPDATE requests SET state = 'execution_uncertain', updated_at = %s"
                               " WHERE state = 'executing'", (now,))
            await conn.execute("UPDATE requests SET state = 'failure', failure_reason = 'restart', updated_at = %s"
                               " WHERE state = 'processing'", (now,))
            await conn.execute("UPDATE call_log SET status = 'abandoned' WHERE status = 'running'")

    async def drain(self) -> None:
        """Wait for background executions (tests)."""
        while self._tasks:
            await asyncio.gather(*list(self._tasks), return_exceptions=True)
