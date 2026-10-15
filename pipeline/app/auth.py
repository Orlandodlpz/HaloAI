"""API keys: one per client, stored as SHA-256 hashes, each with a scope and a rate limit."""

import math
import secrets
from dataclasses import dataclass

from fastapi import Request

from app import ids
from app.lifecycle import ApiError


@dataclass
class Caller:
    key_id: str
    user_id: str
    client: str
    scope: str  # 'real' (phone) or 'mock' (demo site)
    rate_per_min: int


async def create_key(pool, user_id: str, client: str, scope: str, rate_per_min: int = 60,
                     key: str | None = None) -> str:
    """Store a key's hash and return the key. The plain key is never stored.

    A demo ('mock') key may only belong to a sandbox user and a phone ('real') key never, so the
    demo site can never reach the phone user's memory or calendar."""
    key = key or secrets.token_urlsafe(32)
    async with pool.connection() as conn:
        cur = await conn.execute("SELECT sandbox FROM users WHERE id = %s", (user_id,))
        row = await cur.fetchone()
        if row is None or row["sandbox"] != (scope == "mock"):
            raise ValueError("mock keys need a sandbox user and real keys a normal user")
        await conn.execute(
            "INSERT INTO api_keys (user_id, client, scope, key_hash, rate_per_min) VALUES (%s, %s, %s, %s, %s)"
            " ON CONFLICT (key_hash) DO NOTHING",
            (user_id, client, scope, ids.key_hash(key), rate_per_min))
    return key


async def require_caller(request: Request) -> Caller:
    """Hard constraint: every API request requires the API key."""
    header = request.headers.get("authorization", "")
    scheme, _, token = header.partition(" ")
    if scheme.lower() != "bearer" or not token:
        raise ApiError("unauthorized", 401, "missing API key")
    services = request.app.state.services
    async with services.pool.connection() as conn:
        cur = await conn.execute(
            "SELECT id, user_id, client, scope, rate_per_min FROM api_keys"
            " WHERE key_hash = %s AND revoked_at IS NULL", (ids.key_hash(token),))
        row = await cur.fetchone()
    if row is None:
        raise ApiError("unauthorized", 401, "invalid API key")
    caller = Caller(str(row["id"]), str(row["user_id"]), row["client"], row["scope"], row["rate_per_min"])
    wait_s = services.limiter.check(caller.key_id, caller.rate_per_min)
    if wait_s > 0:
        raise ApiError("rate_limited", 429, "too many requests for this key",
                       headers={"Retry-After": str(max(1, math.ceil(wait_s)))})
    return caller
