"""B0 gate: one plain completion, then N tool-calling runs with a toy tool.

Each tool run passes only if the model returns exactly one call to the toy
tool, its arguments parse as JSON, and they match the schema (required
keys, types, timezone-aware datetime). Reasoning stays off (the default)
and every run checks that no reasoning text came back.

Run: .venv/bin/python scripts/smoke.py [--runs 5]
Exit code 0 only if every run passes.
"""

import argparse
import json
import statistics
import sys
import time
import uuid
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.config import get_settings  # noqa: E402
from app.llm import get_client, model_kwargs  # noqa: E402
from app.timing import hop, request_id_var  # noqa: E402

TOY_TOOL = {
    "type": "function",
    "function": {
        "name": "create_event",
        "description": "Create a calendar event.",
        "parameters": {
            "type": "object",
            "properties": {
                "title": {"type": "string", "description": "Short event title."},
                "start": {
                    "type": "string",
                    "description": "Start time, ISO 8601 with UTC offset, e.g. 2026-10-07T15:00:00-05:00.",
                },
                "duration_minutes": {"type": "integer", "description": "Length in minutes."},
            },
            "required": ["title", "start", "duration_minutes"],
        },
    },
}

SYSTEM = (
    "You turn short spoken commands into tool calls. "
    "Current time: 2026-10-06T15:24:00-05:00 (Tuesday). Timezone: America/Chicago."
)
COMMAND = "block 3 to 4 tomorrow for the gym"


def reasoning_text(message) -> str:
    return (message.model_extra or {}).get("reasoning") or ""


def check_tool_call(message) -> tuple[bool, str, dict | None]:
    calls = message.tool_calls or []
    if len(calls) != 1:
        return False, f"expected 1 tool call, got {len(calls)}", None
    call = calls[0]
    if call.function.name != "create_event":
        return False, f"wrong tool {call.function.name}", None
    try:
        args = json.loads(call.function.arguments)
    except json.JSONDecodeError as e:
        return False, f"arguments not JSON: {e}", None
    if not isinstance(args, dict):
        return False, "arguments not an object", None
    missing = [k for k in ("title", "start", "duration_minutes") if k not in args]
    if missing:
        return False, f"missing {missing}", args
    if not isinstance(args["title"], str) or not args["title"].strip():
        return False, "title not a non-empty string", args
    if not isinstance(args["duration_minutes"], int):
        return False, "duration_minutes not an integer", args
    try:
        start = datetime.fromisoformat(args["start"])
    except ValueError:
        return False, "start not ISO 8601", args
    if start.tzinfo is None:
        return False, "start has no UTC offset", args
    if reasoning_text(message):
        return False, "reasoning text returned while reasoning is off", args
    return True, "ok", args


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=5)
    runs = parser.parse_args().runs

    client = get_client()
    print(f"model: {get_settings().nemotron_model_id}")

    request_id_var.set(f"smoke-plain-{uuid.uuid4().hex[:8]}")
    with hop("nemotron_plain") as f:
        resp = client.chat.completions.create(
            messages=[{"role": "user", "content": "Reply with exactly: HaloAI online"}],
            max_tokens=32,
            **model_kwargs(),
        )
        f["completion_tokens"] = resp.usage.completion_tokens
    plain = resp.choices[0].message
    plain_ok = bool(plain.content and plain.content.strip()) and not reasoning_text(plain)
    print(f"plain completion: {'PASS' if plain_ok else 'FAIL'} {plain.content!r}")

    passed, latencies = 0, []
    for i in range(1, runs + 1):
        request_id_var.set(f"smoke-tool-{i}-{uuid.uuid4().hex[:8]}")
        t0 = time.perf_counter()
        with hop("nemotron_tool_call") as f:
            resp = client.chat.completions.create(
                messages=[
                    {"role": "system", "content": SYSTEM},
                    {"role": "user", "content": COMMAND},
                ],
                tools=[TOY_TOOL],
                tool_choice="auto",
                max_tokens=256,
                **model_kwargs(),
            )
            f["prompt_tokens"] = resp.usage.prompt_tokens
            f["completion_tokens"] = resp.usage.completion_tokens
        latencies.append(time.perf_counter() - t0)
        ok, why, args = check_tool_call(resp.choices[0].message)
        passed += ok
        print(f"tool run {i}: {'PASS' if ok else 'FAIL'} ({why}) {json.dumps(args)}")

    print(
        f"tool calls: {passed}/{runs} passed; latency median "
        f"{statistics.median(latencies):.2f}s, max {max(latencies):.2f}s"
    )
    return 0 if plain_ok and passed == runs else 1


if __name__ == "__main__":
    sys.exit(main())
