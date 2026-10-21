"""Create an API key for a client. The key is printed once and only its hash is stored.

Run: .venv/bin/python scripts/create_key.py --client demo-site --scope mock
     .venv/bin/python scripts/create_key.py --client phone-carlos-dev --scope mock --user phone-dev
     .venv/bin/python scripts/create_key.py --client phone-carlos --scope real --rate 120
     .venv/bin/python scripts/create_key.py --client carlos-google-test --scope real --user carlos-google-test
Scope 'mock' can only ever reach sample data; 'real' can reach real tools (the user's Google account).
--user gives a key its own user. For mock keys: a developer's own sample world, memory and outbox,
separate from the demo site's (2026-10-19). For real keys: a separate real user with its own Google
connection, so connect, status and disconnect can be tested with a developer's own Google account
without touching the demo account, which belongs to the default 'demo' user (2026-10-21). The
developer's Google address must be a test user of the OAuth app (Google Cloud console).
"""

import argparse
import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.auth import create_key  # noqa: E402
from app.config import get_settings  # noqa: E402
from app.db import migrate, open_pool  # noqa: E402


async def main(client: str, scope: str, rate: int, user: str | None) -> None:
    url = get_settings().database_url.get_secret_value()
    await migrate(url)
    pool = await open_pool(url)
    try:
        # Phone keys belong to the 'demo' user, demo site keys to the 'demo-site' sandbox; --user picks
        # another. create_key refuses a real key on a sandbox user and a mock key on a real one.
        if scope == "mock":
            name, sandbox = (user or "demo-site"), True
        else:
            name, sandbox = (user or "demo"), False
        async with pool.connection() as conn:
            cur = await conn.execute(
                "INSERT INTO users (name, sandbox) VALUES (%s, %s) ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.name"
                " RETURNING id", (name, sandbox))
            user_id = str((await cur.fetchone())["id"])
        key = await create_key(pool, user_id, client, scope, rate)
    finally:
        await pool.close()
    print(f"client: {client}  scope: {scope}  rate: {rate}/min")
    print(f"key (shown once, store it outside git): {key}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--scope", required=True, choices=["real", "mock"])
    parser.add_argument("--rate", type=int, default=60)
    parser.add_argument("--user", help="the key's own user: e.g. phone-dev (mock) or carlos-google-test (real)")
    args = parser.parse_args()
    asyncio.run(main(args.client, args.scope, args.rate, args.user))
