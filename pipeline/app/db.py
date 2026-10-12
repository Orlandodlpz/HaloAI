"""Postgres connection pool and the migration runner.

Migrations are plain numbered SQL files in pipeline/migrations, applied in order,
each in its own transaction, and recorded in schema_migrations.
"""

from pathlib import Path

from psycopg import AsyncConnection
from psycopg.rows import dict_row
from psycopg_pool import AsyncConnectionPool

from app.config import PIPELINE_DIR

MIGRATIONS_DIR = PIPELINE_DIR / "migrations"


async def open_pool(url: str, min_size: int = 1, max_size: int = 10) -> AsyncConnectionPool:
    pool = AsyncConnectionPool(
        url,
        min_size=min_size,
        max_size=max_size,
        kwargs={"row_factory": dict_row},
        open=False,
    )
    await pool.open(wait=True)
    return pool


async def migrate(url: str, migrations_dir: Path = MIGRATIONS_DIR) -> list[str]:
    """Apply pending migrations. Returns the versions applied in this run."""
    applied: list[str] = []
    async with await AsyncConnection.connect(url, autocommit=True) as conn:
        await conn.execute(
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            " version text PRIMARY KEY, applied_at timestamptz NOT NULL DEFAULT now())"
        )
        cur = await conn.execute("SELECT version FROM schema_migrations")
        done = {row[0] for row in await cur.fetchall()}
        for path in sorted(migrations_dir.glob("*.sql")):
            version = path.stem
            if version in done:
                continue
            async with conn.transaction():
                await conn.execute(path.read_text())
                await conn.execute(
                    "INSERT INTO schema_migrations (version) VALUES (%s)", (version,)
                )
            applied.append(version)
    return applied
