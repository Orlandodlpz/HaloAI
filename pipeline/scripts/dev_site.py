"""Run the demo site locally, the way Caddy serves it on the server (B6).

Serves pipeline/site/, answers /config.js with the DEMO_SITE_KEY from .env (never written to
disk), and forwards /v1/ to the local backend, so the page and the API share one origin.

    # terminal 1: the backend, with a demo key registered at startup
    cd pipeline && PROCESSOR=nemotron .venv/bin/uvicorn app.main:app --workers 1
    # terminal 2: this
    cd pipeline && .venv/bin/python scripts/dev_site.py      # then open http://127.0.0.1:8080

Needs DEMO_SITE_KEY in pipeline/.env (any random string for local use).
"""

import json
import os
import sys
from pathlib import Path

import httpx
import uvicorn
from starlette.applications import Starlette
from starlette.requests import Request
from starlette.responses import Response, StreamingResponse
from starlette.routing import Mount, Route
from starlette.staticfiles import StaticFiles

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from app.config import get_settings  # noqa: E402

SITE = Path(__file__).resolve().parent.parent / "site"
BACKEND = os.environ.get("HALOAI_BACKEND", "http://127.0.0.1:8000")
client = httpx.AsyncClient(base_url=BACKEND, timeout=60)


async def config(_: Request) -> Response:
    key = get_settings().demo_site_key
    if key is None:
        return Response("// DEMO_SITE_KEY is not set in pipeline/.env\n", media_type="text/javascript")
    body = f"window.HALO_CONFIG = {json.dumps({'demoKey': key.get_secret_value()})};\n"
    return Response(body, media_type="text/javascript")


async def proxy(request: Request) -> StreamingResponse:
    req = client.build_request(request.method, request.url.path, params=request.query_params,
                               headers={k: v for k, v in request.headers.items() if k.lower() != "host"},
                               content=await request.body())
    resp = await client.send(req, stream=True)  # streamed, so Server-Sent Events arrive as they happen
    skip = ("content-length", "content-encoding", "transfer-encoding")
    return StreamingResponse(resp.aiter_raw(), status_code=resp.status_code,
                             headers={k: v for k, v in resp.headers.items() if k.lower() not in skip})


app = Starlette(routes=[Route("/config.js", config),
                        Route("/v1/{path:path}", proxy, methods=["GET", "POST", "PATCH", "DELETE"]),
                        Mount("/", StaticFiles(directory=SITE, html=True))])

if __name__ == "__main__":
    print(f"demo site on http://127.0.0.1:8080, API forwarded to {BACKEND}")
    uvicorn.run(app, host="127.0.0.1", port=8080, log_level="warning")
