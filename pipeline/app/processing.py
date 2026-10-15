"""Interfaces between the request lifecycle and the parts that do real work.

Processor: turns a request's inputs into a result (B2: the Nemotron tool loop).
Executor: carries out an approved proposal (B4: Google Calendar and Gmail).
B1 uses the fakes below so the lifecycle can be built and tested first.
"""

import asyncio
import time
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from datetime import datetime


@dataclass
class Segment:
    revision: int
    kind: str  # 'voice' or 'select'
    text: str | None
    candidate_id: str | None
    sent_at: datetime | None


@dataclass
class ProcessContext:
    request_id: str
    revision: int
    segments: list[Segment]
    timezone: str
    language: str
    orientation: str
    scope: str
    on_status: Callable[[str], Awaitable[None]]
    user_id: str = ""  # whose mock calendar and memory to use (empty in evals: frozen sample world)


@dataclass
class ProcessorOutput:
    state: str  # conflict_ambiguous | conflict_schedule | review | read_only_result
    screen_lines: list[str]
    draft: dict | None = None
    candidates: list[dict] | None = None
    review_screen_lines: list[str] | None = None  # conflict_schedule: shown after continue
    timings: dict = field(default_factory=dict)
    memory_writes: list[dict] = field(default_factory=list)  # "remember that": saved only if this result is stored


@dataclass
class ExecutionOutcome:
    applied: bool
    screen_lines: list[str]
    detail: dict = field(default_factory=dict)


class Processor:
    async def process(self, ctx: ProcessContext) -> ProcessorOutput:
        raise NotImplementedError


class Executor:
    name = "base"

    async def execute(self, draft: dict, execution_key: str, user_id: str = "") -> ExecutionOutcome:
        raise NotImplementedError


class FakeProcessor(Processor):
    """Scripted results from keywords in the voice text, for lifecycle tests.

    'ambiguous' -> two candidates; 'nobody' -> no candidates; 'conflict' -> schedule
    conflict; a question -> read-only result; 'explode' -> error; anything else -> review.
    A selection always leads to review. delay_s and gate let tests control timing."""

    def __init__(self, delay_s: float = 0.0):
        self.delay_s = delay_s
        self.gate: asyncio.Event | None = None  # when set by a test, processing waits for it
        self.calls: list[ProcessContext] = []

    async def process(self, ctx: ProcessContext) -> ProcessorOutput:
        self.calls.append(ctx)
        t0 = time.perf_counter()
        await ctx.on_status("Working on it")
        if self.gate is not None:
            await self.gate.wait()
        if self.delay_s:
            await asyncio.sleep(self.delay_s)
        voice = " ".join(s.text or "" for s in ctx.segments if s.kind == "voice").lower()
        last = ctx.segments[-1]
        timings = {"model_ms": round((time.perf_counter() - t0) * 1000), "tools_ms": 0}

        if "explode" in voice:
            raise RuntimeError("fake processor failure")
        if last.kind == "select":
            draft = {"kind": "email", "to": f"{last.candidate_id}@example.com", "body": voice}
            return ProcessorOutput("review", [f"To: {draft['to']}", "Re: Note"], draft=draft, timings=timings)
        if "nobody" in voice:
            return ProcessorOutput("conflict_ambiguous", ["Who should get this?", "Hold and say the address"], candidates=[], timings=timings)
        if "ambiguous" in voice:
            candidates = [
                {"candidate_id": "victor_t", "label": "Victor T."},
                {"candidate_id": "victor_r", "label": "Victor R."},
            ]
            return ProcessorOutput("conflict_ambiguous", ["Which Victor?"], candidates=candidates, timings=timings)
        if "conflict" in voice:
            draft = {"kind": "event", "title": "Gym", "start": "2026-10-13T15:00:00-05:00", "minutes": 60}
            return ProcessorOutput(
                "conflict_schedule", ["Overlaps: Team sync", "Press to keep anyway"],
                draft=draft, review_screen_lines=["Gym", "Tue 3:00 to 4:00 PM"], timings=timings,
            )
        if voice.strip().endswith("?") or voice.startswith("what"):
            return ProcessorOutput("read_only_result", ["You have 2 events today"], timings=timings)
        draft = {"kind": "event", "title": voice[:40] or "Event", "start": "2026-10-13T15:00:00-05:00", "minutes": 60}
        return ProcessorOutput("review", [draft["title"], "Tue 3:00 to 4:00 PM"], draft=draft, timings=timings)


class FakeExecutor(Executor):
    """mode: 'succeed', 'fail' (confirmed not applied), 'error' (raises: outcome unknown).
    delay_s simulates a slow Google call; hang=True never returns."""

    def __init__(self, name: str = "mock", mode: str = "succeed", delay_s: float = 0.0, hang: bool = False):
        self.name = name
        self.mode = mode
        self.delay_s = delay_s
        self.hang = hang
        self.calls: list[str] = []

    async def execute(self, draft: dict, execution_key: str, user_id: str = "") -> ExecutionOutcome:
        self.calls.append(execution_key)
        if self.hang:
            await asyncio.Event().wait()
        if self.delay_s:
            await asyncio.sleep(self.delay_s)
        if self.mode == "error":
            raise RuntimeError("fake executor error")
        if self.mode == "fail":
            return ExecutionOutcome(False, ["Not sent", "The server said no"])
        return ExecutionOutcome(True, ["Done", draft.get("title") or draft.get("to") or ""])
