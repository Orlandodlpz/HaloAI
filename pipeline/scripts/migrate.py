"""Apply pending database migrations.

Run: .venv/bin/python scripts/migrate.py            (development database)
     .venv/bin/python scripts/migrate.py --test     (test database)
"""

import asyncio
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.config import get_settings  # noqa: E402
from app.db import migrate  # noqa: E402


def main() -> None:
    settings = get_settings()
    if "--test" in sys.argv:
        url = os.environ.get("TEST_DATABASE_URL") or settings.test_database_url.get_secret_value()
    else:
        url = settings.database_url.get_secret_value()
    applied = asyncio.run(migrate(url))
    print(f"applied: {applied or 'nothing, already up to date'}")


if __name__ == "__main__":
    main()
