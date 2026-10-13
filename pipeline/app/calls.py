"""Idempotent retries by call_id (BRIEFING section 2, "Retries").

The first call with a call_id runs; any repeat gets the stored answer instead of
running again. A repeat that arrives while the first is still running waits for it.
Same call_id with a different body is a conflict. Rows are kept 24 h (config).
"""

import asyncio
import time
from dataclasses import dataclass
from datetime import timedelta

from psycopg.types.json import Jsonb

from app import ids
from app.lifecycle import ApiError


@dataclass
class CallStart:
    replay: bool
    http_status: int | None = None
    response: dict | None = None
    running: bool = False  # streaming retry while the first call still runs: reattach
    abandoned: bool = False  # the first call died before storing an answer


class Calls:
    def __init__(self, pool, clock, settings):
        self.pool = pool
        self.clock = clock
        self.settings = settings
        self._waiters: dict[str, asyncio.Future] = {}

    async def begin(self, user_id: str, call_id: str, path: str, body: dict,
                    wait_if_running: bool = True) -> CallStart:
        bh = ids.body_hash(path, body)
        now = self.clock.now()
        async with self.pool.connection() as conn, conn.transaction():
            await conn.execute("DELETE FROM call_log WHERE created_at < %s",
                               (now - timedelta(hours=self.settings.call_retention_h),))
            cur = await conn.execute(
                "INSERT INTO call_log (call_id, user_id, path, body_hash, status, created_at)"
                " VALUES (%s, %s, %s, %s, 'running', %s) ON CONFLICT (call_id) DO NOTHING RETURNING call_id",
                (call_id, user_id, path, bh, now))
            if await cur.fetchone():
                self._waiters[call_id] = asyncio.get_running_loop().create_future()
                return CallStart(replay=False)
            cur = await conn.execute("SELECT * FROM call_log WHERE call_id = %s FOR UPDATE", (call_id,))
            row = await cur.fetchone()
            if str(row["user_id"]) != user_id or row["body_hash"] != bh:
                raise ApiError("call_id_conflict", 409, "call_id reused with a different request")
            if row["status"] == "done":
                return CallStart(True, row["http_status"], row["response"])
            waiter = self._waiters.get(call_id)
            if waiter is None:
                # Abandoned (crash or error before an answer was stored). Every JSON call is
                # safe to run again: approval dedupes by execution key, the rest change nothing twice.
                await conn.execute("UPDATE call_log SET status = 'running' WHERE call_id = %s", (call_id,))
                self._waiters[call_id] = asyncio.get_running_loop().create_future()
                return CallStart(replay=False, abandoned=True)
            if not wait_if_running:
                return CallStart(replay=False, running=True)
        # Still running in this process: wait for the first call's answer.
        http_status, response = await asyncio.shield(waiter)
        return CallStart(True, http_status, response)

    async def finish(self, call_id: str, http_status: int, response: dict) -> None:
        async with self.pool.connection() as conn:
            await conn.execute(
                "UPDATE call_log SET status = 'done', http_status = %s, response = %s WHERE call_id = %s",
                (http_status, Jsonb(response), call_id))
        waiter = self._waiters.pop(call_id, None)
        if waiter and not waiter.done():
            waiter.set_result((http_status, response))

    async def abandon(self, call_id: str) -> None:
        async with self.pool.connection() as conn:
            await conn.execute("UPDATE call_log SET status = 'abandoned' WHERE call_id = %s", (call_id,))
        waiter = self._waiters.pop(call_id, None)
        if waiter and not waiter.done():
            waiter.set_exception(ApiError("upstream_error", 502, "the original call failed"))


class RateLimiter:
    """Token bucket per API key: rate_per_min calls, refilled continuously.
    In memory, which is correct only with a single uvicorn worker."""

    def __init__(self):
        self._buckets: dict[str, tuple[float, float]] = {}

    def check(self, key_id: str, rate_per_min: int) -> float:
        """0 if this call is allowed (and counted), otherwise the seconds until the next one is."""
        now = time.monotonic()
        tokens, last = self._buckets.get(key_id, (float(rate_per_min), now))
        tokens = min(float(rate_per_min), tokens + (now - last) * rate_per_min / 60.0)
        if tokens < 1.0:
            self._buckets[key_id] = (tokens, now)
            return (1.0 - tokens) * 60.0 / rate_per_min
        self._buckets[key_id] = (tokens - 1.0, now)
        return 0.0
