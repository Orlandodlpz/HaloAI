"""Per-hop timestamp logging.

Each hop writes one JSON line: request_id, hop name, start time (UTC,
RFC 3339) and duration in ms. Device and phone log their own hops; joining
on request_id attributes latency across the whole path.
"""

import json
import logging
import sys
import time
from contextlib import contextmanager
from contextvars import ContextVar
from datetime import datetime, timezone

request_id_var: ContextVar[str] = ContextVar("request_id", default="-")

logger = logging.getLogger("haloai.timing")
if not logger.handlers:
    _handler = logging.StreamHandler(sys.stdout)
    _handler.setFormatter(logging.Formatter("%(message)s"))
    logger.addHandler(_handler)
    logger.setLevel(logging.INFO)
    logger.propagate = False


@contextmanager
def hop(name: str, **extra):
    """Time a block. Extra fields (token counts, status) can be added to
    the yielded dict and are logged with the hop."""
    started_at = datetime.now(timezone.utc)
    t0 = time.perf_counter()
    fields: dict = dict(extra)
    ok = True
    try:
        yield fields
    except Exception:
        ok = False
        raise
    finally:
        record = {
            "request_id": request_id_var.get(),
            "hop": name,
            "start": started_at.isoformat(timespec="milliseconds"),
            "duration_ms": round((time.perf_counter() - t0) * 1000, 1),
            "ok": ok,
            **fields,
        }
        logger.info(json.dumps(record))
