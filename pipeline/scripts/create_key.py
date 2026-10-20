"""Create an API key for a client. The key is printed once and only its hash is stored.

Run: .venv/bin/python scripts/create_key.py --client demo-site --scope mock
     .venv/bin/python scripts/create_key.py --client phone-carlos-dev --scope mock --user phone-dev
     .venv/bin/python scripts/create_key.py --client phone-carlos --scope real --rate 120
Scope 'mock' can only ever reach sample data; 'real' can reach real tools (the phone's Google account).
--user gives a mock key its own sandbox user, so a developer's sample world, memory and outbox are
separate from the demo site's (for building the phone app against the live server, 2026-10-19).
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
        # Phone keys belong to the 'demo' user; demo site keys to the separate 'demo-site' sandbox.
        name, sandbox = ((user or "demo-site"), True) if scope == "mock" else ("demo", False)
        if user and scope != "mock":
            raise SystemExit("--user is only for mock keys; real keys belong to the 'demo' user")
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
    parser.add_argument("--user", help="mock keys only: their own sandbox user, e.g. phone-dev")
    args = parser.parse_args()
    asyncio.run(main(args.client, args.scope, args.rate, args.user))
