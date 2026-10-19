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
from app.memory import remember_intent
from app.mockdata import MockWorld, build_world
from app.processing import ProcessContext, Processor, ProcessorOutput
from app.prompt import system_prompt, user_message
from app.screens import budget
from app.timing import hop
from app.tools import (READ_TOOLS, STATUS, TOOL_SPECS, Final, ReadResult, ToolError, compact_speech, run_tool,
                       untrusted)


NO_SEARCH_TOOLS = [t for t in TOOL_SPECS if t["function"]["name"] != "web_search"]

# The model asking "would you like me to add it?" instead of acting (seen live after a web search,
# 2026-10-19). The device's REVIEW screen is the confirmation, so the code sends it back once.
ASKS_TO_CONFIRM = re.compile(r"\b(would you like me to|do you want me to|shall i|should i|if you want|if you'd like"
                             r"|i can (propose|add|create|set up|book|schedule|send|draft))\b", re.IGNORECASE)
ACT_DIRECTLY = ("Do not ask for confirmation: the device shows every proposal for approval. If the request "
                "asks for an action, call the tool now. Otherwise answer briefly.")


def plain_text(text: str) -> str:
    """Model text for an e-ink screen: no Markdown symbols, no special spaces, no thinking."""
    # Token Factory returns reasoning in its own field; this only guards against it arriving inline.
    text = re.sub(r"<think>.*?(</think>|$)", "", text, flags=re.DOTALL)
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

    def _model_kwargs(self, reasoning: bool) -> dict:
        s = self.settings
        kwargs = {"model": s.nemotron_model_id,
                  "max_tokens": s.max_reasoning_tokens if reasoning else s.max_output_tokens,
                  "extra_body": {"chat_template_kwargs": {"enable_thinking": reasoning}}}
        if reasoning:
            kwargs.update(temperature=s.reasoning_temperature, top_p=s.reasoning_top_p)
        else:
            kwargs["temperature"] = 0.0  # model card: greedy when reasoning is off
        return kwargs

    async def process(self, ctx: ProcessContext) -> ProcessorOutput:
        tz = ZoneInfo(ctx.timezone)
        said = [s.sent_at for s in ctx.segments if s.sent_at is not None]
        now = max(said).astimezone(tz) if said else datetime.now(tz)  # when the user last spoke
        world = await self.world_factory(ctx, now, tz)
        world.remember_allowed = remember_intent(ctx.segments)  # the user's own words, never a tool result
        request_text = " ".join(s.text or "" for s in ctx.segments if s.kind == "voice")
        world.heard = compact_speech(request_text)  # addresses the user said themselves (tools._untraced)
        chars, lines = budget(self.settings, ctx.orientation)
        answer_chars = min(self.settings.max_answer_chars, chars * (lines - 1))
        messages = [
            {"role": "system", "content": system_prompt(now, ctx.timezone, world, answer_chars=answer_chars,
                                                        request_text=request_text)},
            {"role": "user", "content": user_message(ctx.segments, world)},
        ]
        model_ms = tools_ms = 0.0
        searched = False  # reasoning turns on for the turns after a web search (multi-step)
        nudged = False  # ACT_DIRECTLY sent once already
        searches = 0

        turn = 0
        while turn < self.settings.max_model_turns + (self.settings.extra_turns_after_search if searched else 0):
            turn += 1
            t0 = time.perf_counter()
            reasoning = self.settings.reasoning_enabled or (searched and self.settings.reasoning_after_search)
            with hop("nemotron_turn", turn=turn, reasoning=reasoning) as f:
                # Once the searches are used up, web_search is not offered at all: told "no more searches",
                # the model kept asking anyway until the turn cap (eval trap_06 on the server, 2026-10-19).
                tools = TOOL_SPECS if searches < self.settings.max_searches else NO_SEARCH_TOOLS
                resp = await self.client.chat.completions.create(
                    messages=messages, tools=tools, tool_choice="auto", **self._model_kwargs(reasoning))
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
                limit = self.settings.max_model_turns + (self.settings.extra_turns_after_search if searched else 0)
                if ASKS_TO_CONFIRM.search(text) and not nudged and turn < limit:
                    nudged = True
                    messages += [{"role": "assistant", "content": msg.content or ""},
                                 {"role": "user", "content": ACT_DIRECTLY}]
                    continue
                # One screen line per line of the answer: a list of events keeps its line breaks on the
                # badge (the wrapper flattens whitespace inside a line). Runs of blank lines become one.
                lines = re.sub(r"\n{3,}", "\n\n", text).split("\n")
                return ProcessorOutput("read_only_result", lines, timings=timings)

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
                    if name == "web_search":
                        searches += 1
                        if searches > self.settings.max_searches:
                            raise ToolError("no more searches for this request: act with the results you have, "
                                            "or say you could not find it")
                    if name in STATUS:
                        await ctx.on_status(STATUS[name])
                    with hop(f"tool_{name}"):
                        if name == "web_search" and world.web is not None:  # real search is a network call
                            result = ReadResult({"results": await world.web(str(args.get("query", "")))})
                        else:
                            result = run_tool(name, args, world, now,
                                              {"model_ms": round(model_ms), "tools_ms": round(tools_ms)})
                except ToolError as err:
                    content = json.dumps({"error": str(err)})
                else:
                    if isinstance(result, Final):
                        tools_ms += (time.perf_counter() - t1) * 1000
                        result.output.timings = {"model_ms": round(model_ms), "tools_ms": round(tools_ms)}
                        if result.output.draft is not None and world.source != "mock":
                            # Which world it was made from: decides which executor may run it.
                            result.output.draft["source"] = world.source
                        if result.output.draft and result.output.draft.get("kind") == "send_email" \
                                and result.output.draft.get("to_name"):
                            await ctx.on_status(f"Email to {result.output.draft['to_name']}")
                        return result.output
                    content = untrusted(result.data)
                    searched = searched or name == "web_search"
                tools_ms += (time.perf_counter() - t1) * 1000
                messages.append({"role": "tool", "tool_call_id": call.id, "content": content})

        raise RuntimeError(f"no result after {turn} model turns")
