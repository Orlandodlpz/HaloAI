import uuid

import pytest
from psycopg import AsyncConnection

from app.clock import FakeClock
from app.config import get_settings
from app.db import migrate, open_pool
from app.lifecycle import Lifecycle
from app.processing import FakeExecutor, FakeProcessor


@pytest.fixture
async def db_url():
    """A fresh, empty test database schema for every test."""
    settings = get_settings()
    if settings.test_database_url is None:
        pytest.skip("TEST_DATABASE_URL not set")
    url = settings.test_database_url.get_secret_value()
    async with await AsyncConnection.connect(url, autocommit=True) as conn:
        await conn.execute("DROP SCHEMA public CASCADE")
        await conn.execute("CREATE SCHEMA public")
    await migrate(url)
    return url


@pytest.fixture
async def pool(db_url):
    p = await open_pool(db_url, min_size=6, max_size=10)  # real overlap for race tests
    yield p
    await p.close()


@pytest.fixture
def settings():
    # Short waits so timing tests run in well under a second.
    return get_settings().model_copy(update={
        "approve_wait_s": 0.2, "execution_deadline_s": 0.5, "processing_deadline_s": 0.5,
    })


@pytest.fixture
def clock():
    return FakeClock()


@pytest.fixture
def processor():
    return FakeProcessor()


@pytest.fixture
def executors():
    return {"mock": FakeExecutor("mock"), "real": FakeExecutor("real")}


@pytest.fixture
async def lc(pool, settings, clock, processor, executors):
    from app.memory import MemoryStore
    life = Lifecycle(pool, settings, clock, processor, executors, memory=MemoryStore(pool, clock))
    yield life
    await life.drain()


@pytest.fixture
async def user(lc):
    return await lc.ensure_user("demo")


def start_body(text: str = "block 3 to 4 tomorrow for the gym", **extra) -> dict:
    return {
        "request_id": str(uuid.uuid4()), "segment_id": str(uuid.uuid4()), "text": text,
        "sent_at": "2026-10-12T15:00:00-05:00", "timezone": "America/Chicago",
        "language": "en", "orientation": "landscape", **extra,
    }


def addition_body(text: str, revision: int) -> dict:
    return {"segment_id": str(uuid.uuid4()), "text": text, "revision": revision,
            "sent_at": "2026-10-12T15:00:30-05:00"}


@pytest.fixture
async def services(pool, settings, clock, lc):
    from app.calls import Calls, RateLimiter
    from app.main import Services
    from app.streams import Streams
    s = Services(settings, pool, lc, Calls(pool, clock, settings), RateLimiter(), Streams())
    yield s
    await s.streams.drain()


@pytest.fixture
async def demo_user(lc):
    return await lc.ensure_user("demo-site", sandbox=True)


@pytest.fixture
async def keys(pool, user, demo_user):
    from app.auth import create_key
    return {
        "phone": await create_key(pool, user, "phone", "real"),
        "demo": await create_key(pool, demo_user, "demo-site", "mock"),
        "slow": await create_key(pool, user, "limited", "real", rate_per_min=3),
    }


@pytest.fixture
async def client(services):
    import httpx

    from app.main import create_app
    app = create_app(services)
    async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as c:
        yield c


def auth(key: str) -> dict:
    return {"Authorization": f"Bearer {key}"}
