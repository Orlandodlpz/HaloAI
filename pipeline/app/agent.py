"""NemotronProcessor: the plain tool-calling loop (B2). No agent framework.

One request = up to max_model_turns model calls. Each turn the model either:
- calls read tools (calendar, contacts, web, memory): they run, results go back as data;
- calls a proposal tool: the loop ends with a proposal, conflict or question;
- answers in plain text: the loop ends with a read-only result.
A bad call goes back to the model with the reason. Too many turns is a failure.
"""

import json
import re
import time
from collections.abc import Awaitable, Callable
from datetime import datetime
from zoneinfo import ZoneInfo

from app.config import Settings
from app.mockdata import MockWorld, build_world
from app.processing import ProcessContext, Processor, ProcessorOutput
from app.prompt import system_prompt, user_message
from app.screens import budget
from app.timing import hop
from app.tools import READ_TOOLS, STATUS, TOOL_SPECS, Final, ToolError, run_tool, untrusted


def plain_text(text: str) -> str:
    """Model text for an e-ink screen: no Markdown symbols, no special spaces."""
    text = re.sub(r"[\u00a0\u2007\u2009\u202f]", " ", text)  # no-break and thin spaces
    # The e-ink font is plain ASCII: long dashes, curly quotes and ellipses become their ASCII forms.
    text = text.translate(str.maketrans({"\u2013": "-", "\u2014": "-", "\u2018": "'", "\u2019": "'",
                                         "\u201c": '"', "\u201d": '"'})).replace("\u2026", "...")
    text = re.sub(r"(\*\*|__|`)", "", text)                   # bold markers and code ticks
    text = re.sub(r"^\s*#+\s*", "", text, flags=re.MULTILINE)  # heading marks
    text = re.sub(r"^\s*[-*]\s+", "", text, flags=re.MULTILINE)  # bullet marks
    return re.sub(r"[ \t]+", " ", text).strip()


async def frozen_world(ctx: ProcessContext, now: datetime, tz: ZoneInfo) -> MockWorld:
    """The in-memory sample world, rebuilt for each request (evals and tests)."""
    return build_world(now, tz)


class NemotronProcessor(Processor):
    def __init__(self, client, settings: Settings,
                 world_factory: Callable[[ProcessContext, datetime, ZoneInfo], Awaitable[MockWorld]] = frozen_world):
        self.client = client  # AsyncOpenAI-compatible: client.chat.completions.create(...)
        self.settings = settings
        # Where the calendar and contacts come from: the frozen sample world (evals), the user's
        # stored mock world (server, B3), or real Google tools (B4), all behind the same shape.
        self.world_factory = world_factory

    def _model_kwargs(self) -> dict:
        s = self.settings
        kwargs = {"model": s.nemotron_model_id, "max_tokens": s.max_output_tokens,
                  "extra_body": {"chat_template_kwargs": {"enable_thinking": s.reasoning_enabled}}}
        if not s.reasoning_enabled:
            kwargs["temperature"] = 0.0  # model card: greedy when reasoning is off
        return kwargs

    async def process(self, ctx: ProcessContext) -> ProcessorOutput:
        tz = ZoneInfo(ctx.timezone)
        said = [s.sent_at for s in ctx.segments if s.sent_at is not None]
        now = max(said).astimezone(tz) if said else datetime.now(tz)  # when the user last spoke
        world = await self.world_factory(ctx, now, tz)
        chars, lines = budget(self.settings, ctx.orientation)
        answer_chars = min(self.settings.max_answer_chars, chars * (lines - 1))
        messages = [
            {"role": "system", "content": system_prompt(now, ctx.timezone, world, answer_chars=answer_chars)},
            {"role": "user", "content": user_message(ctx.segments, world)},
        ]
        model_ms = tools_ms = 0.0

        for turn in range(1, self.settings.max_model_turns + 1):
            t0 = time.perf_counter()
            with hop("nemotron_turn", turn=turn) as f:
                resp = await self.client.chat.completions.create(
                    messages=messages, tools=TOOL_SPECS, tool_choice="auto", **self._model_kwargs())
                usage = getattr(resp, "usage", None)
                if usage is not None:
                    f["prompt_tokens"], f["completion_tokens"] = usage.prompt_tokens, usage.completion_tokens
            model_ms += (time.perf_counter() - t0) * 1000
            msg = resp.choices[0].message
            calls = msg.tool_calls or []
            timings = {"model_ms": round(model_ms), "tools_ms": round(tools_ms)}

            if not calls:
                text = plain_text(msg.content or "")
                if not text:
                    raise RuntimeError("model returned neither a tool call nor text")
                return ProcessorOutput("read_only_result", [text], timings=timings)

            messages.append({"role": "assistant", "content": msg.content or "", "tool_calls": [
                {"id": c.id, "type": "function", "function": {"name": c.function.name, "arguments": c.function.arguments}}
                for c in calls]})

            # A proposal ends the request, even if read calls came with it. One action per request.
            ordered = sorted(calls, key=lambda c: c.function.name in READ_TOOLS)
            for call in ordered:
                name = call.function.name
                t1 = time.perf_counter()
                try:
                    try:
                        args = json.loads(call.function.arguments or "{}")
                    except json.JSONDecodeError:
                        raise ToolError("arguments were not valid JSON") from None
                    if name in STATUS:
                        await ctx.on_status(STATUS[name])
                    with hop(f"tool_{name}"):
                        result = run_tool(name, args, world, now,
                                          {"model_ms": round(model_ms), "tools_ms": round(tools_ms)})
                except ToolError as err:
                    content = json.dumps({"error": str(err)})
                else:
                    if isinstance(result, Final):
                        tools_ms += (time.perf_counter() - t1) * 1000
                        result.output.timings = {"model_ms": round(model_ms), "tools_ms": round(tools_ms)}
                        if result.output.draft and result.output.draft.get("kind") == "send_email" \
                                and result.output.draft.get("to_name"):
                            await ctx.on_status(f"Email to {result.output.draft['to_name']}")
                        return result.output
                    content = untrusted(result.data)
                tools_ms += (time.perf_counter() - t1) * 1000
                messages.append({"role": "tool", "tool_call_id": call.id, "content": content})

        raise RuntimeError(f"no result after {self.settings.max_model_turns} model turns")
