"""FastAPI app factory.

Run with ONE worker (uvicorn app.main:app). Retries, stream reattach and rate limits
keep state in this process; a second worker would not see it.
"""

import logging
import uuid
from contextlib import asynccontextmanager
from dataclasses import dataclass

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from app.api import error_payload, router
from app.auth import create_key
from app.calls import Calls, RateLimiter
from app.clock import SystemClock
from app.config import Settings, get_settings
from app.db import migrate, open_pool
from app.lifecycle import ApiError, Lifecycle
from app.memory import MemoryStore
from app.google_api import router as google_router
from app.google_auth import GoogleAuth
from app.memory_api import router as memory_router
from app.mockstore import MockExecutor, MockStore
from app.processing import FakeExecutor, FakeProcessor, Processor
from app.streams import Streams
from app.streams import router as stream_router
from app.timing import hop, request_id_var

log = logging.getLogger("haloai")


@dataclass
class Services:
    settings: Settings
    pool: object
    lc: Lifecycle
    calls: Calls
    limiter: RateLimiter
    streams: Streams
    google: object = None  # GoogleAuth (B4)


def make_processor(settings: Settings, store: MockStore, memory: MemoryStore) -> Processor:
    """'fake' (B1 keyword script) or 'nemotron' (the real loop), from the PROCESSOR setting."""
    if settings.processor == "nemotron":
        from app.agent import NemotronProcessor
        from app.llm import get_async_client

        async def user_world(ctx, now, tz):
            world = await store.world(ctx.user_id, now, tz)  # each user's stored mock calendar
            snapshot = await memory.load(ctx.user_id)        # and what the assistant remembers
            world.memory_items, world.actions = snapshot.items, snapshot.actions
            return world
        return NemotronProcessor(get_async_client(), settings, world_factory=user_world)
    if settings.processor == "fake":
        return FakeProcessor()
    raise ValueError(f"unknown PROCESSOR {settings.processor!r}; use 'fake' or 'nemotron'")


async def build_services(settings: Settings) -> Services:
    url = settings.database_url.get_secret_value()
    await migrate(url)
    pool = await open_pool(url)
    clock = SystemClock()
    # Executors stay fake until B4 (real Google tools).
    store = MockStore(pool, clock)
    memory = MemoryStore(pool, clock)
    # Approved actions change the user's mock world. The B1 fake processor's drafts are not real
    # drafts, so it keeps the fake executor. Real tools come in B4.
    mock_exec = MockExecutor(store) if settings.processor == "nemotron" else FakeExecutor("mock")
    lc = Lifecycle(pool, settings, clock, make_processor(settings, store, memory),
                   {"mock": mock_exec, "real": FakeExecutor("real")}, memory=memory)
    user_id = await lc.ensure_user("demo")
    # The phone key from .env, so the existing key keeps working. More keys: scripts/create_key.py
    await create_key(pool, user_id, "phone", "real", key=settings.backend_api_key.get_secret_value())
    await lc.recover_on_startup()
    return Services(settings, pool, lc, Calls(pool, clock, settings), RateLimiter(), Streams(),
                    GoogleAuth(pool, settings, clock))


def create_app(services: Services | None = None) -> FastAPI:
    @asynccontextmanager
    async def lifespan(app: FastAPI):
        if services is not None:  # tests inject their own
            yield
            return
        app.state.services = await build_services(get_settings())
        try:
            yield
        finally:
            await app.state.services.pool.close()

    # No built-in docs pages: they answer without an API key, and every API request needs one.
    app = FastAPI(title="HaloAI pipeline", version="0.2.0", lifespan=lifespan,
                  docs_url=None, redoc_url=None, openapi_url=None)
    if services is not None:
        app.state.services = services

    @app.exception_handler(ApiError)
    async def api_error(_request: Request, err: ApiError):
        return JSONResponse(error_payload(err), status_code=err.http_status, headers=err.headers)

    @app.exception_handler(RequestValidationError)
    async def validation_error(_request: Request, err: RequestValidationError):
        detail = "; ".join(f"{'.'.join(str(p) for p in e['loc'])}: {e['msg']}" for e in err.errors())
        return JSONResponse(error_payload(ApiError("invalid_request", 422, detail)), status_code=422)

    @app.middleware("http")
    async def time_request(request: Request, call_next):
        request_id_var.set(request.headers.get("x-request-id") or str(uuid.uuid4()))
        with hop("backend_request", method=request.method, path=request.url.path) as f:
            response = await call_next(request)
            f["status"] = response.status_code
        return response

    app.include_router(router)
    app.include_router(stream_router)
    app.include_router(memory_router)
    app.include_router(google_router)
    return app


app = create_app()
