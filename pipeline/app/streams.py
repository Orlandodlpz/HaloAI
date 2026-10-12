"""Streaming endpoints: start, additions, select (BRIEFING section 2, "Status updates").

Event order on every stream: one `accepted`, zero or more `status`, an optional `slow`,
then exactly one `result`. Every event carries request_id and revision. A `: ping`
comment every 10 s (config) keeps cellular connections open.

The work runs in a background task that publishes into an in-memory hub. If the phone's
connection drops, the work continues; a retry with the same call_id reattaches to the
hub while it runs (accepted, latest status, then the rest) or replays the stored
accepted and result once it has finished.
"""

import asyncio
import json
import time
from collections.abc import AsyncIterator, Awaitable, Callable
from typing import Literal
from uuid import UUID
from zoneinfo import ZoneInfo

from fastapi import APIRouter, Depends, Request
from fastapi.responses import JSONResponse
from pydantic import AwareDatetime, BaseModel, Field, field_validator
from sse_starlette import EventSourceResponse, ServerSentEvent

from app.api import error_payload
from app.auth import Caller, require_caller
from app.lifecycle import ApiError
from app.timing import hop, request_id_var

router = APIRouter()
Event = tuple[str, dict]


class StartBody(BaseModel):
    call_id: UUID
    request_id: UUID
    segment_id: UUID
    text: str = Field(min_length=1, max_length=4000)
    sent_at: AwareDatetime  # must carry a UTC offset
    timezone: str
    language: str = Field(min_length=2, max_length=16)
    orientation: Literal["landscape", "portrait"]

    @field_validator("timezone")
    @classmethod
    def iana_zone(cls, value: str) -> str:
        # ZoneInfo raises a KeyError subclass, which pydantic would not report as a 422.
        try:
            ZoneInfo(value)
        except (KeyError, ValueError):
            raise ValueError("timezone must be an IANA name such as America/Chicago") from None
        return value


class AdditionBody(BaseModel):
    call_id: UUID
    revision: int = Field(ge=1)  # the base revision the phone saw
    segment_id: UUID
    text: str = Field(min_length=1, max_length=4000)
    sent_at: AwareDatetime


class SelectBody(BaseModel):
    call_id: UUID
    revision: int = Field(ge=1)
    candidate_id: str = Field(min_length=1, max_length=200)


class StreamHub:
    """Events of one streaming call, shared by the first connection and any reattach."""

    def __init__(self):
        self.events: list[Event] = []
        self.done = False
        self._cond = asyncio.Condition()

    async def publish(self, event: str, data: dict) -> None:
        async with self._cond:
            self.events.append((event, data))
            self._cond.notify_all()

    async def close(self) -> None:
        async with self._cond:
            self.done = True
            self._cond.notify_all()

    async def follow(self, reattach: bool = False) -> AsyncIterator[Event]:
        async with self._cond:
            if reattach:  # accepted, the latest status (and slow), then whatever comes next
                snap = list(self.events)
                initial = snap[:1] + [e for e in snap if e[0] == "status"][-1:] \
                    + [e for e in snap if e[0] in ("slow", "result")]
                index = len(snap)
            else:
                initial, index = [], 0
        for event in initial:
            yield event
        while True:
            async with self._cond:
                while index >= len(self.events) and not self.done:
                    await self._cond.wait()
                batch, index, done = self.events[index:], len(self.events), self.done
            for event in batch:
                yield event
            if done and index >= len(self.events):
                return


class Streams:
    def __init__(self):
        self.hubs: dict[str, StreamHub] = {}
        self.tasks: set[asyncio.Task] = set()

    async def drain(self) -> None:
        while self.tasks:
            await asyncio.gather(*list(self.tasks), return_exceptions=True)


def sse(events: AsyncIterator[Event] | list[Event], settings) -> EventSourceResponse:
    async def gen():
        if isinstance(events, list):
            for event, data in events:
                yield ServerSentEvent(data=json.dumps(data), event=event, sep="\n")
        else:
            async for event, data in events:
                yield ServerSentEvent(data=json.dumps(data), event=event, sep="\n")

    return EventSourceResponse(
        gen(), ping=settings.sse_ping_s, sep="\n",
        ping_message_factory=lambda: ServerSentEvent(comment="ping", sep="\n"),
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},  # nginx must not buffer
    )


async def stream_call(request: Request, caller: Caller, name: str, body: BaseModel, request_id: str,
                      begin: Callable[[], Awaitable[int]]):
    services = request.app.state.services
    settings, lc, calls, streams = services.settings, services.lc, services.calls, services.streams
    t0 = time.perf_counter()
    data = body.model_dump(mode="json")
    call_id = data["call_id"]
    request_id_var.set(request_id)

    start = await calls.begin(caller.user_id, call_id, request.url.path, data, wait_if_running=False)
    if start.replay:
        if start.http_status != 200:
            return JSONResponse(start.response, status_code=start.http_status)
        return sse([tuple(e) for e in start.response["events"]], settings)
    if start.running and call_id in streams.hubs:
        return sse(streams.hubs[call_id].follow(reattach=True), settings)
    if start.abandoned or start.running:
        # The first call died before finishing. Never redo the input: answer with the
        # request's current state if the input was already recorded.
        try:
            current = await lc.get_result(caller.user_id, request_id)
        except ApiError:
            current = None
        if current is not None:
            events = [("accepted", {"request_id": request_id, "revision": current["revision"], "call_id": call_id}),
                      ("result", current)]
            await calls.finish(call_id, 200, {"events": events})
            return sse(events, settings)

    try:
        revision = await begin()
    except ApiError as err:  # refused before any work started: a plain JSON error, stored for retries
        payload = error_payload(err)
        await calls.finish(call_id, err.http_status, payload)
        return JSONResponse(payload, status_code=err.http_status)
    except Exception:
        await calls.abandon(call_id)
        raise

    hub = StreamHub()
    streams.hubs[call_id] = hub
    accepted = {"request_id": request_id, "revision": revision, "call_id": call_id}
    await hub.publish("accepted", accepted)

    async def run():
        result_ready = asyncio.Event()

        async def on_status(line: str) -> None:
            await hub.publish("status", {"request_id": request_id, "revision": revision, "status_line": line})

        async def slow_watch() -> None:
            try:
                await asyncio.wait_for(result_ready.wait(), settings.slow_threshold_s)
            except TimeoutError:
                await hub.publish("slow", {"request_id": request_id, "revision": revision})

        watcher = asyncio.create_task(slow_watch())
        try:
            with hop(f"api_{name}_process", client=caller.client, revision=revision) as f:
                result = await lc.process(request_id, revision, on_status)
                f["state"] = result["state"]
            result_ready.set()
            result["timings"] = {**result.get("timings", {}),
                                 "backend_total_ms": round((time.perf_counter() - t0) * 1000)}
            await hub.publish("result", result)
            await calls.finish(call_id, 200, {"events": [["accepted", accepted], ["result", result]]})
        except Exception:
            await calls.abandon(call_id)
            raise
        finally:
            result_ready.set()
            watcher.cancel()
            await hub.close()
            streams.hubs.pop(call_id, None)

    task = asyncio.create_task(run())
    streams.tasks.add(task)
    task.add_done_callback(streams.tasks.discard)
    return sse(hub.follow(), settings)


@router.post("/v1/requests")
async def start_request(body: StartBody, request: Request, caller: Caller = Depends(require_caller)):
    lc = request.app.state.services.lc
    payload = body.model_dump(mode="json")
    return await stream_call(request, caller, "start", body, str(body.request_id),
                             lambda: lc.start(caller.user_id, caller.scope, payload))


@router.post("/v1/requests/{request_id}/additions")
async def add_to_request(request_id: UUID, body: AdditionBody, request: Request,
                         caller: Caller = Depends(require_caller)):
    lc = request.app.state.services.lc
    payload = body.model_dump(mode="json")
    return await stream_call(request, caller, "addition", body, str(request_id),
                             lambda: lc.add(caller.user_id, str(request_id), payload))


@router.post("/v1/requests/{request_id}/select")
async def select_candidate(request_id: UUID, body: SelectBody, request: Request,
                           caller: Caller = Depends(require_caller)):
    lc = request.app.state.services.lc
    payload = body.model_dump(mode="json")
    return await stream_call(request, caller, "select", body, str(request_id),
                             lambda: lc.select(caller.user_id, str(request_id), payload))
