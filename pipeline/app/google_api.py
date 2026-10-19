"""Google account endpoints (BRIEFING section 2, signed off by Carlos 2026-10-19).

status, connect and disconnect need the API key like every endpoint. The callback is the one
exception (Google calls it); it is protected by the one time state in app/google_auth.py.
Demo site keys (sandbox users) can never connect a Google account."""

import html
from uuid import UUID

from fastapi import APIRouter, Depends, Request
from fastapi.responses import HTMLResponse
from pydantic import BaseModel

from app.api import json_call
from app.auth import Caller, require_caller
from app.google_auth import GoogleError
from app.lifecycle import ApiError
from app.timing import hop

router = APIRouter()


class CallBody(BaseModel):
    call_id: UUID


def _google(request: Request):
    return request.app.state.services.google


def _page(title: str, message: str, status: int) -> HTMLResponse:
    body = (f"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
            f"<title>HaloAI</title><style>body{{font-family:system-ui,sans-serif;max-width:32rem;margin:4rem auto;"
            f"padding:0 1rem;line-height:1.5}}</style></head><body><h1>{html.escape(title)}</h1>"
            f"<p>{html.escape(message)}</p></body></html>")
    return HTMLResponse(body, status_code=status)


@router.get("/v1/google/status")
async def google_status(request: Request, caller: Caller = Depends(require_caller)):
    return await json_call(request, caller, "google_status", None, lambda: _google(request).status(caller.user_id))


@router.post("/v1/google/connect")
async def google_connect(body: CallBody, request: Request, caller: Caller = Depends(require_caller)):
    async def handler():
        if caller.scope != "real":
            raise ApiError("invalid_request", 422, "the demo site cannot connect a Google account")
        try:
            return {"url": await _google(request).connect_url(caller.user_id)}
        except GoogleError as err:
            raise ApiError("upstream_error", 502, str(err)) from None
    return await json_call(request, caller, "google_connect", body, handler)


@router.post("/v1/google/disconnect")
async def google_disconnect(body: CallBody, request: Request, caller: Caller = Depends(require_caller)):
    return await json_call(request, caller, "google_disconnect", body,
                           lambda: _google(request).disconnect(caller.user_id))


@router.get("/v1/google/callback", include_in_schema=False)
async def google_callback(request: Request, state: str = "", code: str = "", error: str = ""):
    """Called by Google after the user signs in, not by the phone. No API key: the state proves who."""
    with hop("google_callback") as f:
        if error or not state or not code:
            f["result"] = error or "missing"
            return _page("Not connected", "Google sign in was cancelled or failed. Start again from the HaloAI app.", 400)
        try:
            email = await _google(request).callback(state, code)
        except GoogleError as err:
            f["result"] = "refused"
            return _page("Not connected", str(err), 400)
        f["result"] = "connected"
        return _page("Connected", f"HaloAI is connected to {email}. You can close this page and go back to the app.", 200)
