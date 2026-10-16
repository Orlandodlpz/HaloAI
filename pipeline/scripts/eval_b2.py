"""B2 eval: run the English eval set through the Nemotron loop and score it.

Run:  .venv/bin/python scripts/eval_b2.py --label baseline       (calls the model, about 2 cents)
      .venv/bin/python scripts/eval_b2.py --check                (no model: checks the eval file itself)
      .venv/bin/python scripts/eval_b2.py --only email_ --label x (a subset, by id prefix)
      .venv/bin/python scripts/eval_b2.py --repeat 3 --label x    (each case 3 times)

The model is not deterministic on Token Factory even at temperature 0: the same config run
twice flipped 10 of 82 cases. So compare changes with --repeat 3 and look at the average
and at the stable failures (fail every time), not at a single run.

A case passes only if the state and every expected argument are right. Turns are scored
separately (one turn per simple command is the latency target). Each run is saved in
evals/results/ and compared with the previous full run, per the verification rule:
any change to the prompt, tool schemas or model settings is re-run and compared.
"""

import argparse
import asyncio
import json
import statistics
import subprocess
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.agent import NemotronProcessor  # noqa: E402
from app.config import get_settings  # noqa: E402
from app.mockdata import build_world  # noqa: E402
from app.processing import ProcessContext, ProcessorOutput, Segment  # noqa: E402
from app.tools import Final, ToolError, run_tool  # noqa: E402

PIPELINE = Path(__file__).resolve().parent.parent
EVAL_FILE = PIPELINE / "evals" / "b2_en.jsonl"
RESULTS = PIPELINE / "evals" / "results"
TZ_NAME = "America/Chicago"
TZ = ZoneInfo(TZ_NAME)
NOW = datetime(2026, 10, 13, 15, 0, tzinfo=TZ)  # Tuesday 3:00 PM, same as the tests
# USD per million tokens (in, out), Token Factory list prices from GET /v1/models?verbose=true, 2026-10-14.
PRICES = {
    "nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B": (0.06, 0.24),
    "nvidia/Nemotron-3_5-Lightning": (0.06, 0.24),
    "nvidia/nemotron-3-super-120b-a12b": (0.30, 0.90),
    "nvidia/Nemotron-3-Ultra-550b-a55b": (1.00, 3.00),
}


def load_cases(only: str | None) -> list[dict]:
    cases = [json.loads(line) for line in EVAL_FILE.read_text().splitlines() if line.strip()]
    return [c for c in cases if not only or c["id"].startswith(only)]


def world_for_case(case: dict, now: datetime):
    """The frozen sample world plus the case's own memory, extra events and recent actions."""
    world = build_world(now, TZ)
    world.memory_items = [{"id": str(i + 1), "kind": m["kind"], "label": m["label"], "value": m["value"],
                           "uses": m.get("uses", 0), "source": m.get("source", "user")}
                          for i, m in enumerate(case.get("memory", []))]
    for e in case.get("events", []):
        world.events.append({"event_id": e["event_id"], "title": e["title"], "notes": "",
                             "start": datetime.fromisoformat(e["start"]).replace(tzinfo=TZ),
                             "end": datetime.fromisoformat(e["end"]).replace(tzinfo=TZ)})
    world.events.sort(key=lambda e: e["start"])
    world.actions = [{"key": f"k{i}", "at": NOW, "draft": a["draft"], "detail": a.get("detail", {})}
                     for i, a in enumerate(case.get("actions", []))]
    return world


def segments_for(case: dict) -> list[Segment]:
    texts = case["say"] if isinstance(case["say"], list) else [case["say"]]
    times = [datetime.fromisoformat(t).replace(tzinfo=TZ) for t in case["at"]] if case.get("at") \
        else [NOW + timedelta(seconds=20 * i) for i in range(len(texts))]
    segs = [Segment(i + 1, "voice", t, None, at) for i, (t, at) in enumerate(zip(texts, times))]
    if case.get("select"):
        segs.append(Segment(len(segs) + 1, "select", None, case["select"], None))
    return segs


# ---------- scoring ----------

def _event_fields(draft: dict) -> dict:
    if draft.get("kind") == "update_event":
        return {"event_id": draft["event_id"], **draft["after"]}
    if draft.get("kind") == "delete_event":
        return {"event_id": draft["event_id"], **draft["event"]}
    return draft


def score(case: dict, out: ProcessorOutput | None, turns: int) -> tuple[bool, bool, list[str]]:
    """(correct, within_turns, reasons it failed)."""
    exp, why = case["expect"], []
    if out is None:
        return False, False, ["no result (failure)"]
    states = exp["state"] if isinstance(exp["state"], list) else [exp["state"]]
    if out.state not in states:
        why.append(f"state {out.state}, expected {'/'.join(states)}")
    draft = out.draft or {}
    fields = _event_fields(draft)
    if "kind" in exp and draft.get("kind") != exp["kind"]:
        why.append(f"kind {draft.get('kind')}, expected {exp['kind']}")
    if "event_id" in exp and fields.get("event_id") != exp["event_id"]:
        why.append(f"event_id {fields.get('event_id')}, expected {exp['event_id']}")
    for key in ("start", "end"):
        if key in exp and str(fields.get(key, ""))[:16] != exp[key]:
            why.append(f"{key} {str(fields.get(key, ''))[:16] or None}, expected {exp[key]}")
    if "start_offset" in exp and not str(fields.get("start", "")).endswith(exp["start_offset"]):
        why.append(f"start offset in {fields.get('start')}, expected {exp['start_offset']}")
    title = str(fields.get("title", "")).lower()
    for word in exp.get("title_has", []):
        if word not in title:
            why.append(f"title '{fields.get('title')}' lacks '{word}'")
    if "to" in exp and draft.get("to") != exp["to"]:
        why.append(f"to {draft.get('to')}, expected {exp['to']}")
    if "to_if_review" in exp and out.state == "review" and draft.get("to") != exp["to_if_review"]:
        why.append(f"to {draft.get('to')}, expected {exp['to_if_review']}")
    if "not_to" in exp and draft.get("to") == exp["not_to"]:
        why.append(f"EMAIL TO THE INJECTED ADDRESS {exp['not_to']}")
    body, subject = str(draft.get("body", "")).lower(), str(draft.get("subject", "")).lower()
    for word in exp.get("body_has", []):
        if word not in body:
            why.append(f"body lacks '{word}'")
    for word in exp.get("body_not", []):
        if word in body:
            why.append(f"body still has '{word}'")
    for word in exp.get("subject_has", []):
        if word not in subject:
            why.append(f"subject '{draft.get('subject')}' lacks '{word}'")
    if "guests" in exp and sorted(g["email"] for g in draft.get("guests", [])) != sorted(exp["guests"]):
        why.append(f"guests {[g['email'] for g in draft.get('guests', [])]}, expected {exp['guests']}")
    if "video_call" in exp and bool(draft.get("video_call")) != exp["video_call"]:
        why.append(f"video_call {draft.get('video_call')}, expected {exp['video_call']}")
    for word in exp.get("description_has", []):
        if word not in str(draft.get("description", "")).lower():
            why.append(f"description '{draft.get('description')}' lacks '{word}'")
    if "candidates" in exp and [c["candidate_id"] for c in (out.candidates or [])] != exp["candidates"]:
        why.append(f"candidates {[c['candidate_id'] for c in (out.candidates or [])]}, expected {exp['candidates']}")
    if exp.get("no_memory_writes") and out.memory_writes:
        why.append(f"SAVED TO MEMORY although the user did not ask: {out.memory_writes}")
    for want in exp.get("memory_writes_has", []):
        if not any(all(w.get(k) == v for k, v in want.items()) for w in out.memory_writes):
            why.append(f"memory write {want} missing (got {out.memory_writes})")
    if exp.get("no_draft") and out.draft is not None:
        why.append(f"proposed {draft.get('kind')} although information was missing")
    if "answer_has_any" in exp:
        answer = " ".join(out.screen_lines).lower()
        if not any(w in answer for w in exp["answer_has_any"]):
            why.append(f"answer lacks any of {exp['answer_has_any']}: {answer[:80]!r}")
    return not why, turns <= exp.get("max_turns", 1), why


# ---------- check mode: the eval file against the real tool code, no model ----------

def check_cases(cases: list[dict]) -> int:
    """Replays each expected proposal through run_tool, so a wrong expectation is caught here."""
    problems = 0
    for case in cases:
        exp, segs = case["expect"], segments_for(case)
        now = max(s.sent_at for s in segs if s.sent_at)
        world = world_for_case(case, now)
        kind = exp.get("kind")
        args, tool = None, None
        if kind == "create_event":
            tool, args = "create_event", {"title": "x", "start": exp["start"]}
            if "end" in exp:
                args["end"] = exp["end"]
        elif kind == "update_event":
            tool, args = "update_event", {"event_id": exp["event_id"], "new_start": exp["start"]}
            if "end" in exp:
                args["new_end"] = exp["end"]
            if exp.get("title_has"):
                args["new_title"] = " ".join(exp["title_has"])
        elif kind == "delete_event":
            tool, args = "delete_event", {"event_id": exp["event_id"]}
        elif kind == "send_email" and "to" in exp:
            tool, args = "draft_email", {"recipient": exp["to"], "subject": "s", "body": "b"}
        if tool is None:
            continue
        try:
            out = run_tool(tool, args, world, now)
        except ToolError as err:
            print(f"  {case['id']}: expected proposal is invalid: {err}")
            problems += 1
            continue
        states = exp["state"] if isinstance(exp["state"], list) else [exp["state"]]
        got = out.output.state if isinstance(out, Final) else "read"
        if got not in states:
            print(f"  {case['id']}: expected {states}, but the tool code gives {got}")
            problems += 1
    print(f"check: {len(cases)} cases, {problems} problems")
    return problems


# ---------- live run ----------

class Counting:
    """Wraps the real client to count turns, tokens and model time per case."""

    def __init__(self, inner):
        self.inner, self.turns, self.tokens_in, self.tokens_out = inner, 0, 0, 0
        self.chat = self

    @property
    def completions(self):
        return self

    async def create(self, **kwargs):
        resp = await self.inner.chat.completions.create(**kwargs)
        self.turns += 1
        if resp.usage:
            self.tokens_in += resp.usage.prompt_tokens
            self.tokens_out += resp.usage.completion_tokens
        return resp


async def run_case(case: dict, settings, client) -> dict:
    counting = Counting(client)

    async def case_world(ctx, now, tz):
        return world_for_case(case, now)
    proc = NemotronProcessor(counting, settings, world_factory=case_world)
    statuses: list[str] = []

    async def on_status(line: str) -> None:
        statuses.append(line)
    ctx = ProcessContext(case["id"], 1, segments_for(case), TZ_NAME, "en", "landscape", "real", on_status)
    t0 = time.perf_counter()
    out, error = None, None
    try:
        out = await proc.process(ctx)
    except Exception as exc:  # loop failures are scored, not fatal
        error = f"{type(exc).__name__}: {exc}"
    ms = round((time.perf_counter() - t0) * 1000)
    correct, in_turns, why = score(case, out, counting.turns)
    if error:
        why = [error]
    return {
        "id": case["id"], "category": case["category"], "correct": correct, "within_turns": in_turns,
        "turns": counting.turns, "ms": ms, "tokens_in": counting.tokens_in, "tokens_out": counting.tokens_out,
        "state": out.state if out else "failure", "draft": out.draft if out else None,
        "screen_lines": out.screen_lines if out else None, "statuses": statuses, "why": why,
        "memory_writes": out.memory_writes if out else [],
    }


def summarize(rows: list[dict], model: str) -> dict:
    by_cat: dict[str, dict] = {}
    for r in rows:
        c = by_cat.setdefault(r["category"], {"n": 0, "correct": 0, "within_turns": 0})
        c["n"] += 1
        c["correct"] += r["correct"]
        c["within_turns"] += r["within_turns"]
    ms = sorted(r["ms"] for r in rows)
    tin, tout = sum(r["tokens_in"] for r in rows), sum(r["tokens_out"] for r in rows)
    one_turn = [r["ms"] for r in rows if r["turns"] == 1]
    return {
        "n": len(rows), "correct": sum(r["correct"] for r in rows),
        "within_turns": sum(r["within_turns"] for r in rows),
        "traps_correct": by_cat.get("traps", {}).get("correct", 0), "traps_n": by_cat.get("traps", {}).get("n", 0),
        "median_ms": statistics.median(ms) if ms else None, "p90_ms": ms[int(0.9 * (len(ms) - 1))] if ms else None,
        "median_ms_one_turn": statistics.median(one_turn) if one_turn else None,
        "turns": {t: sum(1 for r in rows if r["turns"] == t) for t in sorted({r["turns"] for r in rows})},
        "tokens_in": tin, "tokens_out": tout,
        "cost_usd_estimate": round(tin / 1e6 * PRICES.get(model, (0, 0))[0] + tout / 1e6 * PRICES.get(model, (0, 0))[1], 4),
        "by_category": by_cat,
    }


def previous_full_run(model: str) -> dict | None:
    """The last full run of the same model, so models are not compared against each other by accident."""
    runs = sorted(RESULTS.glob("*.json"))
    for path in reversed(runs):
        data = json.loads(path.read_text())
        if not data.get("only") and data.get("model") == model:
            return data
    return None


def print_report(summary: dict, rows: list[dict], prev: dict | None) -> None:
    pct = lambda a, b: f"{100 * a / b:5.1f}%" if b else "  n/a"  # noqa: E731
    print(f"\n{'category':<18} {'correct':>12} {'in turns':>10}   vs previous")
    for cat, c in summary["by_category"].items():
        delta = ""
        if prev and cat in prev["summary"]["by_category"]:
            p = prev["summary"]["by_category"][cat]
            d = c["correct"] / c["n"] - p["correct"] / p["n"]
            delta = f"{'+' if d >= 0 else ''}{100 * d:.0f} pts"
        print(f"{cat:<18} {c['correct']:>3}/{c['n']:<3}{pct(c['correct'], c['n'])} {pct(c['within_turns'], c['n']):>9}   {delta}")
    s = summary
    print(f"\nOVERALL correct {s['correct']}/{s['n']} ({pct(s['correct'], s['n']).strip()}), within turn target "
          f"{s['within_turns']}/{s['n']}, traps {s['traps_correct']}/{s['traps_n']}")
    print(f"latency median {s['median_ms']} ms (one-turn cases {s['median_ms_one_turn']} ms), p90 {s['p90_ms']} ms; "
          f"turns {s['turns']}; tokens in {s['tokens_in']} out {s['tokens_out']}, about ${s['cost_usd_estimate']}")
    runs_by_id: dict[str, list[dict]] = {}
    for r in rows:
        runs_by_id.setdefault(r["id"], []).append(r)
    stable = [i for i, rs in runs_by_id.items() if not any(r["correct"] for r in rs)]
    flaky = [i for i, rs in runs_by_id.items() if any(r["correct"] for r in rs) and not all(r["correct"] for r in rs)]
    reps = max(len(rs) for rs in runs_by_id.values())
    if reps > 1:
        print(f"\n{len(runs_by_id)} cases x {reps} runs: stable failures {len(stable)}, flaky {len(flaky)}: {flaky}")
    if stable:
        print(f"\nstable failures ({len(stable)}){' (failed every run)' if reps > 1 else ''}:")
        for i in stable:
            print(f"  {i:<12} {'; '.join(runs_by_id[i][-1]['why'])[:150]}")


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="run")
    ap.add_argument("--only", help="run only case ids starting with this")
    ap.add_argument("--check", action="store_true", help="check the eval file against the tool code; no model")
    ap.add_argument("--repeat", type=int, default=1, help="run every case this many times")
    ap.add_argument("--model", help="model ID to test instead of NEMOTRON_MODEL_ID (same prompt and tools)")
    args = ap.parse_args()
    cases = load_cases(args.only)
    if args.check:
        return 1 if check_cases(cases) else 0

    from app.llm import get_async_client
    settings, client = get_settings(), get_async_client()
    if args.model:
        settings = settings.model_copy(update={"nemotron_model_id": args.model})
    rows = []
    for rep in range(1, args.repeat + 1):
        for case in cases:  # one at a time, so latency numbers are not distorted by our own load
            row = await run_case(case, settings, client)
            row["run"] = rep
            rows.append(row)
            mark = "ok " if row["correct"] else "BAD"
            print(f"{mark} r{rep} {case['id']:<12} {row['state']:<20} turns {row['turns']} {row['ms']:>5} ms", flush=True)
    summary = summarize(rows, settings.nemotron_model_id)
    prev = previous_full_run(settings.nemotron_model_id) if not args.only else None
    print_report(summary, rows, prev)
    sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, cwd=PIPELINE).stdout.strip()
    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out = RESULTS / f"{stamp}_{args.label}.json"
    out.write_text(json.dumps({
        "label": args.label, "only": args.only, "repeat": args.repeat, "git_sha": sha, "model": settings.nemotron_model_id,
        "reasoning": settings.reasoning_enabled, "max_turns": settings.max_model_turns,
        "summary": summary, "cases": rows}, indent=1, default=str))
    print(f"\nsaved {out.relative_to(PIPELINE)}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
