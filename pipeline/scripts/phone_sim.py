#!/usr/bin/env python3
"""Phone simulator: the calls the HaloAI phone app makes, against the live backend (for P2 and I1).

Runs every flow the phone has to handle, step by step, and prints for each step:
  ->  the exact request sent (method, path, JSON body)
  <-  the exact answer (stream events or JSON)
  BADGE  the screen the badge should show, and the Screen write the phone should send it
         over Bluetooth ({"sid", "screen", "rev", "screen_lines", "candidates", "expires_in_s",
         "pending_key"}, BRIEFING section 2 and phone/docs/ble_fields_draft.md).
When the app behaves differently, compare it with this output line by line.

Standard library only (Python 3.10+). By default it uses the demo site's sample data: it reads the
public demo key from https://haloai-cov.io/config.js and starts its own visitor session, so nothing
real is created or sent and no secret is needed.

    python3 pipeline/scripts/phone_sim.py              # list the scenarios
    python3 pipeline/scripts/phone_sim.py create       # one scenario
    python3 pipeline/scripts/phone_sim.py all          # every scenario except expire (about 1 minute)
    python3 pipeline/scripts/phone_sim.py email --portrait
    python3 pipeline/scripts/phone_sim.py create --key "$BACKEND_API_KEY" --real   # the phone key: REAL Google

Exit code 0 when every step answered as expected, 1 otherwise.
"""

import argparse
import json
import re
import secrets
import sys
import time
import urllib.error
import urllib.request
import uuid
from datetime import datetime

SITE = "https://haloai-cov.io"
SCREEN = {"review": "REVIEW", "conflict_ambiguous": "CONFLICT_AMBIGUOUS", "conflict_schedule": "CONFLICT_SCHEDULE",
          "read_only_result": "READ_ONLY_RESULT", "executing": "EXECUTING", "success": "SUCCESS",
          "failure": "FAILURE", "execution_uncertain": "EXECUTION_UNCERTAIN", "cancelled": "CANCELLED"}
CONFIRM_GUARD_S = 1.1  # the badge ignores a press for about 1 s after REVIEW is drawn


class Unexpected(Exception):
    pass


def now_iso() -> str:
    """Local time with its UTC offset, as the phone must send it (never a bare local time)."""
    return datetime.now().astimezone().isoformat(timespec="seconds")


def short(obj, limit=240) -> str:
    text = json.dumps(obj, ensure_ascii=False)
    return text if len(text) <= limit else text[:limit] + " ..."


class Phone:
    def __init__(self, base: str, token: str, tz: str, orientation: str):
        self.base, self.token, self.tz, self.orientation = base.rstrip("/"), token, tz, orientation
        self.sid = 0
        self.ok = True

    # ---------- HTTP ----------

    def _request(self, method: str, path: str, body: dict | None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method, headers={
            "Authorization": f"Bearer {self.token}", "Content-Type": "application/json",
            "Accept": "text/event-stream, application/json", "User-Agent": "haloai-phone-sim"})
        print(f"  -> {method} {path}" + (f" {short(body)}" if body is not None else ""))
        try:
            return urllib.request.urlopen(req, timeout=40)
        except urllib.error.HTTPError as err:
            return err

    def json(self, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
        resp = self._request(method, path, body)
        status = resp.status if hasattr(resp, "status") else resp.code
        data = json.loads(resp.read() or b"{}")
        extra = f" Retry-After {resp.headers.get('Retry-After')}" if resp.headers.get("Retry-After") else ""
        print(f"  <- {status}{extra} {short(data)}")
        return status, data

    def stream(self, path: str, body: dict) -> dict:
        """POST that answers with Server-Sent Events: accepted, status, slow, then result."""
        resp = self._request("POST", path, body)
        if getattr(resp, "status", None) != 200:
            data = json.loads(resp.read() or b"{}")
            print(f"  <- {resp.code} {short(data)}")
            raise Unexpected(f"stream refused: {data.get('error', {}).get('message')}")
        event, result, t0 = "message", None, time.monotonic()
        for raw in resp:
            line = raw.decode().rstrip("\r\n")
            if line.startswith(":"):
                print(f"  <- {line}   (keepalive ping, ignore)")
            elif line.startswith("event:"):
                event = line[6:].strip()
            elif line.startswith("data:"):
                data = json.loads(line[5:])
                print(f"  <- event {event} +{time.monotonic() - t0:.1f}s {short(data, 160)}")
                if event == "status":
                    self.badge("PROCESSING", data["revision"], status_line=data["status_line"])
                elif event == "slow":
                    self.badge("PROCESSING_SLOW", data["revision"])
                elif event == "result":
                    result = data
        if result is None:
            raise Unexpected("the stream ended without a result")
        self.show(result)
        return result

    # ---------- what the badge shows ----------

    def badge(self, screen: str, rev: int, lines=None, status_line=None, candidates=None, expires=None, key=None):
        self.sid = (self.sid + 1) % 65536
        write = {"sid": self.sid, "screen": screen, "rev": rev}
        if lines is not None:
            write["screen_lines"] = lines
        if status_line is not None:
            write["status_line"] = status_line
        if candidates is not None:
            write["candidates"] = candidates
        if expires is not None:
            write["expires_in_s"] = expires
        if key:
            write["pending_key"] = key
        print(f"  BADGE {screen}" + (f"  (expires in {expires} s)" if expires is not None else ""))
        for line in lines or []:
            print(f"        | {line}")
        for i, c in enumerate(candidates or []):
            print(f"        | [{i}] {c}")
        print(f"  Screen write: {short(write, 400)}")

    def show(self, r: dict):
        screen = SCREEN.get(r["state"], r["state"].upper())
        cands = [c["label"] for c in r["candidates"]] if r.get("candidates") is not None else None
        # pending_key goes with REVIEW (the device saves it as it sends approve), and with
        # EXECUTION_UNCERTAIN after a reconnect (BRIEFING section 2).
        key = r.get("execution_key") if screen in ("REVIEW", "EXECUTION_UNCERTAIN") else None
        self.badge(screen, r["revision"], r.get("screen_lines"), candidates=cands, expires=r.get("expires_in_s"), key=key)
        extra = [f"failure_stage {r['failure_stage']}"] if r.get("failure_stage") else []
        timings = r.get("timings") or {}
        if timings:
            extra.append("timings " + ", ".join(f"{k} {v}" for k, v in timings.items()))
        if extra:
            print("        (" + "; ".join(extra) + ")")

    def expect(self, got: str, *wanted: str):
        if got not in wanted:
            self.ok = False
            print(f"  !! UNEXPECTED: state {got}, expected {' or '.join(wanted)}")
        else:
            print(f"  ok: {got}")

    # ---------- the phone's calls ----------

    def start(self, text: str) -> tuple[str, dict]:
        rid = str(uuid.uuid4())
        r = self.stream("/v1/requests", {"call_id": str(uuid.uuid4()), "request_id": rid, "segment_id": str(uuid.uuid4()),
                                         "text": text, "sent_at": now_iso(), "timezone": self.tz, "language": "en",
                                         "orientation": self.orientation})
        return rid, r

    def addition(self, rid: str, text: str, revision: int) -> dict:
        return self.stream(f"/v1/requests/{rid}/additions", {"call_id": str(uuid.uuid4()), "segment_id": str(uuid.uuid4()),
                                                             "text": text, "revision": revision, "sent_at": now_iso()})

    def select(self, rid: str, r: dict, index: int) -> dict:
        return self.stream(f"/v1/requests/{rid}/select", {"call_id": str(uuid.uuid4()), "revision": r["revision"],
                                                          "candidate_id": r["candidates"][index]["candidate_id"]})

    def approve(self, rid: str, r: dict, call_id: str | None = None) -> tuple[int, dict]:
        print(f"  (user presses after the {CONFIRM_GUARD_S:.1f} s confirm guard)")
        time.sleep(CONFIRM_GUARD_S)
        status, a = self.json("POST", f"/v1/requests/{rid}/approve", {"call_id": call_id or str(uuid.uuid4()),
                                                                      "revision": r["revision"], "proposal_hash": r["proposal_hash"]})
        if status == 200:
            self.show(a)
            for _ in range(30):  # executing: poll the status endpoint about once a second
                if a.get("state") != "executing":
                    break
                time.sleep(1)
                status, a = self.json("GET", f"/v1/executions/{a['execution_key']}")
                self.show(a)
        return status, a

    def cancel(self, rid: str) -> tuple[int, dict]:
        status, c = self.json("POST", f"/v1/requests/{rid}/cancel", {"call_id": str(uuid.uuid4())})
        if status == 200:
            self.show(c)
        return status, c


# ---------- scenarios ----------

def s_create(p: Phone):
    """Calendar: propose, approve after the confirm guard, SUCCESS."""
    rid, r = p.start("block 3 to 4 tomorrow for the gym")
    p.expect(r["state"], "review", "conflict_schedule")
    if r["state"] == "review":
        _, a = p.approve(rid, r)
        p.expect(a.get("state"), "success")
    else:
        p.cancel(rid)


def s_email(p: Phone):
    """Email to a known contact: REVIEW shows the full address, approve, SUCCESS (sample outbox only)."""
    rid, r = p.start("email Victor saying the demo is ready")
    p.expect(r["state"], "review")
    _, a = p.approve(rid, r)
    p.expect(a.get("state"), "success")


def s_choose(p: Phone):
    """Two matching contacts: CONFLICT_AMBIGUOUS with candidates, rotate to the second, press (select)."""
    rid, r = p.start("email Alex about lunch on friday")
    p.expect(r["state"], "conflict_ambiguous")
    if r.get("candidates"):
        r = p.select(rid, r, min(1, len(r["candidates"]) - 1))
        p.expect(r["state"], "review")
    p.cancel(rid)


def s_conflict(p: Phone):
    """Overlap with an existing event: CONFLICT_SCHEDULE, press to continue, REVIEW, then cancel."""
    rid, r = p.start("book a meeting tomorrow at 10 with Carlos")
    p.expect(r["state"], "conflict_schedule", "conflict_ambiguous")
    if r["state"] == "conflict_schedule":
        status, c = p.json("POST", f"/v1/requests/{rid}/continue", {"call_id": str(uuid.uuid4()), "revision": r["revision"]})
        p.show(c)
        p.expect(c.get("state"), "review")
    p.cancel(rid)


def s_addition(p: Phone):
    """Hold in REVIEW: the addition makes revision 2; approving the old revision is refused."""
    rid, r1 = p.start("block 3 to 4 next wednesday for reading")
    p.expect(r1["state"], "review")
    r2 = p.addition(rid, "actually make it 5 to 6", r1["revision"])
    p.expect(r2["state"], "review")
    print(f"  revision {r1['revision']} -> {r2['revision']}; pending_key changed: {r1['execution_key'] != r2['execution_key']}")
    print("  (approving the OLD revision must be refused: the device only approves what it shows)")
    time.sleep(CONFIRM_GUARD_S)
    status, e = p.json("POST", f"/v1/requests/{rid}/approve", {"call_id": str(uuid.uuid4()), "revision": r1["revision"],
                                                               "proposal_hash": r1["proposal_hash"]})
    p.expect(e.get("error", {}).get("code", str(status)), "stale_revision")
    p.cancel(rid)


def s_cancel(p: Phone):
    """Double press in REVIEW: CANCELLED; a late approve is refused, nothing is created."""
    rid, r = p.start("block 9 to 10 next thursday for the dentist follow up")
    p.expect(r["state"], "review", "conflict_schedule")
    _, c = p.cancel(rid)
    p.expect(c.get("state"), "cancelled")
    status, e = p.json("POST", f"/v1/requests/{rid}/approve", {"call_id": str(uuid.uuid4()), "revision": r["revision"],
                                                               "proposal_hash": r["proposal_hash"] or "x"})
    p.expect(str(status), "409")


def s_retry(p: Phone):
    """Approve exactly once: the same call_id replays the stored answer, a new call_id finds the same execution."""
    rid, r = p.start("block 6 to 7 pm next friday for a movie")
    p.expect(r["state"], "review", "conflict_schedule")
    if r["state"] != "review":
        p.cancel(rid)
        return
    cid = str(uuid.uuid4())
    _, first = p.approve(rid, r, call_id=cid)
    print("  (network drop: the phone retries with the SAME call_id)")
    _, again = p.json("POST", f"/v1/requests/{rid}/approve", {"call_id": cid, "revision": r["revision"],
                                                              "proposal_hash": r["proposal_hash"]})
    print("  (app restarted: a NEW call_id for the same approve)")
    _, newer = p.json("POST", f"/v1/requests/{rid}/approve", {"call_id": str(uuid.uuid4()), "revision": r["revision"],
                                                              "proposal_hash": r["proposal_hash"]})
    same = first.get("execution_key") == again.get("execution_key") == newer.get("execution_key")
    print(f"  one execution for all three: {same}")
    p.expect("success" if same and newer.get("state") == "success" else "duplicate", "success")


def s_reconcile(p: Phone):
    """The badge rebooted holding a pending_key: status by key decides; 404 is uncertain, never cancelled."""
    print("  --- a) the device saved the key and sent approve, but the approve never arrived")
    rid, r = p.start("block 7 to 8 pm next tuesday for reading club")
    p.expect(r["state"], "review", "conflict_schedule")
    key = r.get("execution_key")
    if not key:
        p.cancel(rid)
        return
    print(f"  (badge reboots, comes up in RECONCILE STATUS with pending_key {key}, sends status_query)")
    _, s = p.json("GET", f"/v1/executions/{key}")
    p.expect(s.get("state"), "review")
    print("  (still open, so the phone cancels; the cancel reply is final)")
    _, c = p.cancel(rid)
    p.expect(c.get("state"), "cancelled")
    print("  => send CANCELLED to the device in RECONCILE STATUS: nothing happened, the device clears the key")

    print("  --- b) the approve did arrive before the reboot")
    rid, r = p.start("block 8 to 9 pm next tuesday for a walk")
    if r["state"] == "review":
        p.approve(rid, r)
        print("  (badge reboots with that key, sends status_query)")
        _, s = p.json("GET", f"/v1/executions/{r['execution_key']}")
        p.show(s)
        p.expect(s.get("state"), "success")
        print("  => SUCCESS: an authoritative outcome, the device clears the key")
    else:
        p.cancel(rid)

    print("  --- c) a key the backend does not know (only after restoring an old backup)")
    unknown = "".join(secrets.choice("abcdefghijklmnopqrstuvwxyz234567") for _ in range(26))
    status, _ = p.json("GET", f"/v1/executions/{unknown}")
    p.expect(str(status), "404")
    print("  => 404 is NOT cancelled: send EXECUTION_UNCERTAIN, the device keeps the key (section 2, 2026-10-19)")
    p.badge("EXECUTION_UNCERTAIN", 0, ["Not sure it went through", "Check Sent or your calendar"], key=unknown)


def s_expire(p: Phone):
    """60 s with no input: the backend cancels; approve is refused (waits 61 s)."""
    rid, r = p.start("block 4 to 5 next monday for planning")
    p.expect(r["state"], "review", "conflict_schedule")
    print("  (waiting 61 s without any input; rotating the knob would send touch and reset it)")
    time.sleep(61)
    status, e = p.json("POST", f"/v1/requests/{rid}/approve", {"call_id": str(uuid.uuid4()), "revision": r["revision"],
                                                               "proposal_hash": r["proposal_hash"] or "x"})
    p.expect(e.get("error", {}).get("code", str(status)), "expired")
    _, s = p.json("GET", f"/v1/requests/{rid}")
    p.show(s)
    p.expect(s.get("state"), "cancelled")


def s_touch(p: Phone):
    """Knob activity in REVIEW: touch resets the 60 s expiry (no call_id on touch)."""
    rid, r = p.start("block 2 to 3 next tuesday for the library")
    p.expect(r["state"], "review", "conflict_schedule")
    time.sleep(3)
    status, t = p.json("POST", f"/v1/requests/{rid}/touch", {"revision": r["revision"]})
    print(f"  expires_in_s before {r.get('expires_in_s')}, after the touch {t.get('expires_in_s')}")
    p.expect(str(status), "200")
    p.cancel(rid)


def s_question(p: Phone):
    """A question: READ_ONLY_RESULT. Nothing to approve; any input dismisses it."""
    rid, r = p.start("what do I have tomorrow?")
    p.expect(r["state"], "read_only_result")


def s_routine(p: Phone):
    """Routines: save one by voice, then use it by name (ends in REVIEW like any request)."""
    rid, r = p.start("save a routine called gym: block 7 to 8 am for the gym")
    p.expect(r["state"], "read_only_result")
    rid, r = p.start("do my gym routine on friday")
    p.expect(r["state"], "review", "conflict_schedule")
    p.cancel(rid)


SCENARIOS = {"create": s_create, "email": s_email, "choose": s_choose, "conflict": s_conflict,
             "addition": s_addition, "cancel": s_cancel, "retry": s_retry, "reconcile": s_reconcile,
             "touch": s_touch, "question": s_question, "routine": s_routine, "expire": s_expire}


def demo_token(base: str) -> str:
    with urllib.request.urlopen(urllib.request.Request(f"{SITE}/config.js", headers={"User-Agent": "haloai-phone-sim"}),
                                timeout=15) as r:
        m = re.search(r'"demoKey"\s*:\s*"([^"]+)"', r.read().decode())
    if not m:
        sys.exit("could not read the demo key from the site")
    req = urllib.request.Request(f"{base}/v1/demo/session", data=json.dumps({"call_id": str(uuid.uuid4())}).encode(),
                                 method="POST", headers={"Authorization": f"Bearer {m.group(1)}", "Content-Type": "application/json",
                                                         "User-Agent": "haloai-phone-sim"})
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read())["token"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", nargs="?", help="one of: " + ", ".join(SCENARIOS) + ", all")
    ap.add_argument("--base", default="https://api.haloai-cov.io")
    ap.add_argument("--key", help="an API key (default: a demo visitor session, sample data)")
    ap.add_argument("--real", action="store_true", help="confirm that --key is the phone key and REAL actions are fine")
    ap.add_argument("--tz", default=datetime.now().astimezone().tzname() and "America/Chicago",
                    help="IANA timezone sent with every request (default America/Chicago)")
    ap.add_argument("--portrait", action="store_true", help="ask for portrait screen lines (28 x 22)")
    a = ap.parse_args()
    if not a.scenario:
        print(__doc__)
        for name, fn in SCENARIOS.items():
            print(f"  {name:10} {fn.__doc__}")
        return 0
    if a.key and not a.real:
        sys.exit("--key may be the phone key, which creates real events and sends real email. Add --real to confirm.")
    token = a.key or demo_token(a.base)
    print(f"{'REAL account (phone key)' if a.key else 'sample data (demo visitor session)'} at {a.base}, tz {a.tz}\n")
    phone = Phone(a.base, token, a.tz, "portrait" if a.portrait else "landscape")
    names = [n for n in SCENARIOS if n != "expire"] if a.scenario == "all" else [a.scenario]
    for name in names:
        if name not in SCENARIOS:
            sys.exit(f"unknown scenario {name}; one of: {', '.join(SCENARIOS)}, all")
        print(f"=== {name}: {SCENARIOS[name].__doc__}")
        try:
            SCENARIOS[name](phone)
        except Unexpected as err:
            phone.ok = False
            print(f"  !! {err}")
        print()
    print("ALL STEPS AS EXPECTED" if phone.ok else "SOME STEPS WERE UNEXPECTED (marked !!)")
    return 0 if phone.ok else 1


if __name__ == "__main__":
    sys.exit(main())
