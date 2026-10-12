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
from app.processing import FakeExecutor, FakeProcessor
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


async def build_services(settings: Settings) -> Services:
    url = settings.database_url.get_secret_value()
    await migrate(url)
    pool = await open_pool(url)
    clock = SystemClock()
    # B1: fake processor and executors. B2 plugs in the Nemotron loop, B4 the Google tools.
    lc = Lifecycle(pool, settings, clock, FakeProcessor(),
                   {"mock": FakeExecutor("mock"), "real": FakeExecutor("real")})
    user_id = await lc.ensure_user("demo")
    # The phone key from .env, so the existing key keeps working. More keys: scripts/create_key.py
    await create_key(pool, user_id, "phone", "real", key=settings.backend_api_key.get_secret_value())
    await lc.recover_on_startup()
    return Services(settings, pool, lc, Calls(pool, clock, settings), RateLimiter(), Streams())


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

    app = FastAPI(title="HaloAI pipeline", version="0.2.0", lifespan=lifespan)
    if services is not None:
        app.state.services = services

    @app.exception_handler(ApiError)
    async def api_error(_request: Request, err: ApiError):
        return JSONResponse(error_payload(err), status_code=err.http_status)

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
    return app


app = create_app()
