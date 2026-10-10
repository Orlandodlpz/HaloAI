"""FastAPI app. B0: /health only."""

import secrets
import uuid

from fastapi import Depends, FastAPI, HTTPException, Request
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer

from app.config import get_settings
from app.timing import hop, request_id_var

app = FastAPI(title="HaloAI pipeline", version="0.1.0")
bearer = HTTPBearer(auto_error=False)


def require_api_key(creds: HTTPAuthorizationCredentials | None = Depends(bearer)) -> None:
    """Hard constraint: every API request requires the API key."""
    expected = get_settings().backend_api_key.get_secret_value()
    # compare_digest avoids leaking the key through response timing.
    if creds is None or not secrets.compare_digest(creds.credentials, expected):
        raise HTTPException(status_code=401, detail="invalid or missing API key")


@app.middleware("http")
async def time_request(request: Request, call_next):
    # B1 replaces this with the phone's request_id from the JSON body.
    request_id_var.set(request.headers.get("x-request-id") or str(uuid.uuid4()))
    with hop("backend_request", method=request.method, path=request.url.path) as f:
        response = await call_next(request)
        f["status"] = response.status_code
    return response


@app.get("/health", dependencies=[Depends(require_api_key)])
def health() -> dict:
    return {"status": "ok"}
