"""Phone to cloud API, JSON endpoints (BRIEFING section 2).

Streaming endpoints (start, additions, select) are in app/streams.py."""

import time
from uuid import UUID

from fastapi import APIRouter, Depends, Request
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

from app.auth import Caller, require_caller
from app.lifecycle import ApiError
from app.timing import hop, request_id_var

router = APIRouter()


class ContinueBody(BaseModel):
    call_id: UUID
    revision: int = Field(ge=1)


class ApproveBody(BaseModel):
    call_id: UUID
    revision: int = Field(ge=1)
    proposal_hash: str = Field(min_length=1, max_length=128)


class CancelBody(BaseModel):
    call_id: UUID


class TouchBody(BaseModel):
    revision: int = Field(ge=1)


def error_payload(err: ApiError) -> dict:
    return {"error": {"code": err.code, "message": err.message, "current": err.current}}


async def json_call(request: Request, caller: Caller, name: str, body: BaseModel | None, handler) -> JSONResponse:
    """Run a JSON call once per call_id: repeats get the stored answer."""
    services = request.app.state.services
    t0 = time.perf_counter()
    data = body.model_dump(mode="json") if body is not None else {}
    call_id = data.get("call_id")
    if call_id:
        start = await services.calls.begin(caller.user_id, call_id, request.url.path, data)
        if start.replay:
            return JSONResponse(start.response, status_code=start.http_status)
    try:
        with hop(f"api_{name}", client=caller.client) as f:
            try:
                result = await handler()
                if "state" in result:  # request results carry timings; memory responses keep their own shape
                    result["timings"] = {**result.get("timings", {}),
                                         "backend_total_ms": round((time.perf_counter() - t0) * 1000)}
                status, payload = 200, result
            except ApiError as err:
                status, payload = err.http_status, error_payload(err)
            f["status"] = status
    except Exception:
        if call_id:
            await services.calls.abandon(call_id)
        raise
    if call_id:
        await services.calls.finish(call_id, status, payload)
    return JSONResponse(payload, status_code=status)


@router.get("/health")
async def health(caller: Caller = Depends(require_caller)) -> dict:
    return {"status": "ok"}


@router.post("/v1/requests/{request_id}/continue")
async def continue_request(request_id: UUID, body: ContinueBody, request: Request,
                           caller: Caller = Depends(require_caller)):
    request_id_var.set(str(request_id))
    lc = request.app.state.services.lc
    return await json_call(request, caller, "continue", body,
                           lambda: lc.continue_(caller.user_id, str(request_id), {"revision": body.revision}))


@router.post("/v1/requests/{request_id}/approve")
async def approve_request(request_id: UUID, body: ApproveBody, request: Request,
                          caller: Caller = Depends(require_caller)):
    request_id_var.set(str(request_id))
    lc = request.app.state.services.lc
    return await json_call(request, caller, "approve", body, lambda: lc.approve(
        caller.user_id, str(request_id), {"revision": body.revision, "proposal_hash": body.proposal_hash}))


@router.post("/v1/requests/{request_id}/cancel")
async def cancel_request(request_id: UUID, body: CancelBody, request: Request,
                         caller: Caller = Depends(require_caller)):
    request_id_var.set(str(request_id))
    lc = request.app.state.services.lc
    return await json_call(request, caller, "cancel", body, lambda: lc.cancel(caller.user_id, str(request_id)))


@router.post("/v1/requests/{request_id}/touch")
async def touch_request(request_id: UUID, body: TouchBody, request: Request,
                        caller: Caller = Depends(require_caller)):
    # No call_id: resetting a timer twice changes nothing.
    request_id_var.set(str(request_id))
    lc = request.app.state.services.lc
    return await json_call(request, caller, "touch", None,
                           lambda: lc.touch(caller.user_id, str(request_id), body.revision))


@router.get("/v1/requests/{request_id}")
async def request_status(request_id: UUID, request: Request, caller: Caller = Depends(require_caller)):
    """Any request's current result, in any state (asked by Victor, 2026-10-17): read only."""
    lc = request.app.state.services.lc
    return await json_call(request, caller, "request_status", None,
                           lambda: lc.request_status(caller.user_id, str(request_id)))


@router.get("/v1/executions/{execution_key}")
async def execution_status(execution_key: str, request: Request, caller: Caller = Depends(require_caller)):
    lc = request.app.state.services.lc
    return await json_call(request, caller, "execution_status", None,
                           lambda: lc.execution_status(caller.user_id, execution_key))
