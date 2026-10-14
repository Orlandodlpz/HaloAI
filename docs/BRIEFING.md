# BRIEFING.md: HaloAI Execution Plan

**Last Updated:** 2026-10-14
**Purpose:** The operational plan for the whole team: what we're building, how the pieces fit, and the phased sequence to get there with minimal rework.

> Relationship to other docs: CLAUDE.md tracks decisions, constraints and state. This document is the **execution plan**. Each person edits only their own phase section (Backend, Phone or Device); shared sections change only after the team agrees.

## 1. What We're Building

A clip-on voice assistant for calendar and email. Hold the button, speak, check the e-ink screen, confirm with a press.

**Inputs:** the user's voice and button presses on the device.
**Outputs:** events created, moved or deleted in Google Calendar; emails sent through Gmail; short status and confirmation screens on the device.
**Constraint driving every decision:** about 5 seconds from button release to the final screen for a simple command, without ever executing an irreversible action the user did not confirm.

## 2. Integration Map

```
Device (Victor)  --BLE audio, button signals-->  Phone app (Carlos)  --HTTPS text, signals-->  Backend (Orlando)
Device           <--BLE screen text, status----  Phone app           <--HTTPS replies, status--  Backend

Backend --> Nebius Token Factory (Nemotron 3 Nano, Zero Data Retention on)
Backend --> Google Calendar, Gmail send, Google People "other contacts"
Backend --> Tavily web search
Backend --> Postgres (memory, pending actions, history)
Demo site --> Backend (sample data mode uses mock tools)
```

**Who produces and consumes what**
- **Device** produces audio and input events (hold, release, single press, double press, rotate); consumes screens, status updates, chunk acknowledgments, wallpaper and orientation. Owns gesture detection, its local states, the 1 s confirm guard, the WALLPAPER timer and e-ink rendering.
- **Phone** turns audio into text with Parakeet v3, adds date, time, IANA timezone, language and orientation, and talks to the backend. Relays replies and status to the device and signals to the backend. Never sends audio to the cloud.
- **Backend** runs the Nemotron tool-calling loop, owns requests, revisions and proposals, executes approved actions exactly once, answers execution status queries, stores memory and serves the demo site.

**Contracts** (decided 2026-10-06, revised 2026-10-09 for the V1 state machine, signed off 2026-10-12 by Orlando, Carlos and Victor after the API and BLE reviews; summary in CLAUDE.md, "Contracts"). Every message is JSON except the Bluetooth audio channel. Changes need sign-off from the owners of that contract.

*Phone to cloud API.* **Decided 2026-10-12 (Orlando, Carlos).** Base path `/v1/`. Every call carries `Authorization: Bearer <API key>`. Every POST except `touch` carries a phone-generated `call_id` (UUID); GETs and `touch` do not, because repeating them changes nothing. `request_id` (UUID, from the phone) names the user's request across all its revisions; `segment_id` (UUID) names one recording.

```
POST /v1/requests                              (Server-Sent Events)  start a request
{ "call_id": "uuid", "request_id": "uuid", "segment_id": "uuid",
  "text": "send an email to Victor saying ...",
  "sent_at": "2026-10-06T15:24:00-05:00", "timezone": "America/Chicago",
  "language": "en", "orientation": "landscape" }

POST /v1/requests/{request_id}/additions       (Server-Sent Events)  RECORD ADDITION text
{ "call_id": "uuid", "revision": 2, "segment_id": "uuid",
  "text": "make it Thursday instead", "sent_at": "2026-10-06T15:24:40-05:00" }

POST /v1/requests/{request_id}/select          (Server-Sent Events)  CONFLICT-AMBIGUOUS, single press
{ "call_id": "uuid", "revision": 2, "candidate_id": "..." }

POST /v1/requests/{request_id}/continue        CONFLICT-SCHEDULE, continue despite conflict
{ "call_id": "uuid", "revision": 2 }

POST /v1/requests/{request_id}/approve         REVIEW, single press
{ "call_id": "uuid", "revision": 2, "proposal_hash": "..." }

POST /v1/requests/{request_id}/cancel          double press before approval
{ "call_id": "uuid" }

POST /v1/requests/{request_id}/touch           knob activity in a decision state
{ "revision": 2 }

GET  /v1/executions/{execution_key}            RECONCILE STATUS, and polling after approve
```

Every result (the `result` event, and the JSON response of every other call, including the status endpoint) has one shape:

```
{ "state": "conflict_ambiguous | conflict_schedule | review | read_only_result | executing |
            success | failure | execution_uncertain | cancelled",
  "request_id": "...", "revision": 2,
  "screen_lines": ["To: victor@example.com", "Re: Meeting moved", "..."],
  "candidates": [ { "candidate_id": "...", "label": "..." } ] or null,
  "proposal_hash": "..." or null,
  "execution_key": "..." or null,
  "draft": { full email or event, for the phone app } or null,
  "expires_at": "RFC 3339 timestamp" or null,
  "expires_in_s": 60 or null,
  "timings": { "model_ms": 0, "tools_ms": 0, "backend_total_ms": 0 } }
```

An empty `candidates` list means no contact matched: the screen asks the user to hold and say the address. `expires_at` and `expires_in_s` describe the 60 s expiry of a decision state; count down from `expires_in_s` (seconds left when the result was sent), `expires_at` is for logs. `timings` holds the backend's hops for I4; the phone adds its own.

API rules:
- **Revision.** `select`, `continue` and `approve` carry the revision on screen; if it is not the latest, the answer is `409 stale_revision` with the current result. `additions` carry the base revision the phone saw and are never rejected for a stale base: the server adds the segment to the same request (like adding to a conversation, not starting a new one) and assigns the next revision, returned in `accepted`. Only the backend assigns revisions.
- **Retries.** A repeated `call_id` never reruns anything. On a streaming call, a finished call replays `accepted` and the stored `result`; a call still running sends `accepted`, the latest `status`, then the rest of the live stream. Same `call_id` with a different body: `409 call_id_conflict`. `call_id` values are kept for 24 h.
- **Approval exactly once.** The execution key is derived from request, revision and proposal hash, and is short (26 characters) so the device can keep it in flash and send it in `hello`. A second approve for the same request, revision and hash, even with a new `call_id` after an app restart, returns the existing execution's state and never executes again.
- **Approve timing.** Approve waits up to 5 s (config) for Gmail or Calendar, then returns `executing` with `execution_key`, and the phone polls `GET /v1/executions/{execution_key}` about once a second. Timeouts must stay in this order: server wait (5 s) < phone HTTP timeout on approve (10 s) < device EXECUTING timeout.
- **Status endpoint.** Returns the result shape. `state` is `success` (applied), `failure` (not applied), `executing` (still running, keep polling) or `execution_uncertain` (unknown).
- **Cancel.** Before approval: `cancelled`. After approval: `409 already_executing` with the current result, and nothing is cancelled. On a request that is already cancelled or expired: `200` with `cancelled`, so a retried cancel is harmless.
- **One request at a time.** A new `POST /v1/requests` cancels an older request that has not been approved. It is refused with `409 busy` only while the older request's Gmail or Calendar call is running. A finished or uncertain execution blocks nothing.
- **Expiry.** Decision states expire 60 s after the last interaction. Knob rotation reaches the backend as `touch` (from the BLE `activity` event, at most one every 10 s), which resets the timer and returns the result with the new expiry; `additions`, `select` and `continue` also reset it. A `touch` that fails for lack of network loses nothing, since approval needs the network too.
- **Time.** Each segment is read against its own `sent_at`, so "tomorrow" keeps its meaning when an addition after midnight reruns the request.
- **Empty transcripts (phone, no backend call).** On a new request the phone shows FAILURE. On an addition it does not: the phone resends the current screen with the same revision and sends a `touch`, so the proposal survives.
- **Language.** `language` is a hint ("en" until P5); other values are not rejected.
- **API keys.** One key per client, each with a scope: the demo site key works only in sample data mode and can never send real email; the phone key can. In real mode the demo Gmail account sends only to an allowlist of addresses (the team, plus any added for judging). Keys are rate limited per key and can be replaced without code changes. The APK contains its key, so the APK is shared directly, never posted publicly.

Errors have one shape. `message` is for logs only, never for the screen; `current` lets the phone resync without another call. `expired` means the request is no longer open: expired, cancelled or finished; there is no separate `cancelled` code (decided 2026-10-13, Orlando and Carlos).

```
{ "error": { "code": "stale_revision", "message": "...", "current": { result shape } or null } }
```

| HTTP | Codes |
| --- | --- |
| 401 | `unauthorized` |
| 404 | `not_found` (unknown request or execution key) |
| 409 | `stale_revision`, `hash_mismatch`, `expired`, `busy`, `already_executing`, `call_id_conflict` |
| 422 | `invalid_request` |
| 429 | `rate_limited` (too many requests for this key; the response has a `Retry-After` header with the whole seconds to wait, then retry with the same `call_id`. A 429 while polling the status endpoint after approve keeps the device in EXECUTING and polling continues: it is never a failure or uncertain, because the action may be running fine. Decided 2026-10-13, Orlando and Carlos) |
| 502 | `upstream_error` (model, Google or Tavily failed) |

*Status updates.* **Decided 2026-10-12 (Orlando, Carlos).** The start, addition and select calls stream Server-Sent Events, and every event carries `request_id` and `revision`. Order: exactly one `accepted` event first (`{"request_id", "revision", "call_id"}`, with the revision the server assigned), zero or more `status` events (`{"status_line": "Searching the web"}`), a `slow` event if processing passes the configured slow threshold (PROCESSING SLOW), then exactly one `result` event. Once an `accepted` arrives, the phone drops every older stream and any event for an older revision. Only meaningful steps produce a status event. A `: ping` comment every 10 s (config) keeps cellular connections open; the phone uses a read timeout of about 25 s that any bytes reset, and on timeout retries with the same `call_id`.

*Bluetooth (phone and device).* **Decided 2026-10-12 (Carlos, Victor).** One custom BLE service, six characteristics:

| Characteristic | Direction | Format | Carries |
| --- | --- | --- | --- |
| Audio | device to phone (notify) | raw bytes, layout below | 16 kHz mono IMA ADPCM, 20 ms per chunk, streamed from the ring buffer while the button is held |
| Audio Ack | phone to device (write) | JSON | cumulative: `{"req", "seg", "upto"}` (highest sequence number stored with nothing missing before it) every 10 chunks or 200 ms, plus `resend_from` when a chunk is missing; also acknowledges the final marker. The device frees acknowledged chunks |
| Events | device to phone (notify) | JSON | `hello`, `wake`, `record_start`, `record_end`, `approve`, `select` (candidate index), `continue`, `cancel`, `dismiss`, `ack`, `status_query`, `activity` |
| Screen | phone to device (write) | JSON | screen ID `sid`, screen type (one of the V1 state names), `screen_lines`, `status_line`, candidates in display order, revision |
| Wallpaper | phone to device (write) | JSON chunks with offset and checksum, image data base64 | already resized and dithered image |
| Settings | phone to device (write) | JSON | orientation, current time |

Audio chunk: a 16 byte header, little endian, then 160 bytes of IMA ADPCM (320 samples, 20 ms at 16 kHz). 176 bytes per chunk.

| Byte | Field | Notes |
| --- | --- | --- |
| 0 | format | codec ID |
| 1 | flags | bit 0 = final marker; other bits 0 |
| 2 to 3 | req ID | device local request counter |
| 4 | rev | revision the device was displaying when the hold started |
| 5 | seg ID | device local segment counter |
| 6 to 7 | seq | chunk sequence number |
| 8 to 9 | payload length | bytes after the header |
| 10 to 11 | CRC16 | CRC-16/CCITT-FALSE over header bytes 0 to 9 and the payload |
| 12 to 13 | ADPCM predictor | signed 16 bit, decoder state at the start of this chunk |
| 14 | ADPCM step index | decoder state at the start of this chunk |
| 15 | reserved | 0 |

Every chunk carries its ADPCM state, so it decodes on its own and a lost chunk does not corrupt the ones after it. The final marker uses the same header with flag bit 0 set and a 6 byte payload: total chunks (unsigned 16 bit), then total samples (unsigned 32 bit). The final marker, not `record_end`, decides that a recording is complete; `record_end` is informational.

BLE rules:
- **IDs.** The req ID and seg ID in the audio header are device local counters; the phone maps them to the API's `request_id` and `segment_id` UUIDs. The device sends the revision it was displaying when the hold started; only the backend assigns new revisions.
- **MTU and PHY.** The phone requests an MTU of 517; the device's preferred MTU is 256 (the NimBLE default); the smaller value wins. A 176 byte chunk needs a negotiated ATT MTU of at least 179 (3 bytes of ATT header). Below that the connection fails loudly instead of streaming, so there is no MTU adaptive logic. Request the 2M PHY if the chip supports it.
- **Ring buffer.** 2 s of audio on the device (100 chunks, about 17.6 KB), provisional until tested in F2. If the link stalls beyond it, capture stops with FAILURE; audio is never silently dropped.
- **`hello`.** Sent by the device on every connect: firmware version, current state, req ID and any pending execution key. The phone uses it to resync after a reconnect.
- **Screen IDs.** Every Screen write carries a new `sid`. `approve`, `select` and `continue` echo the `sid` of the screen they acted on, and `select` sends the candidate's index on that screen. The phone maps `sid` to revision and proposal hash, and the index to `candidate_id`, so approval binds to exactly what was drawn and no hashes cross BLE.
- **`activity`.** Sent on knob rotation in a decision state, at most one every 10 s; the phone forwards it as `touch`.
- **Large JSON.** JSON messages bigger than one packet are split with a sequence number. Exact JSON field names for the BLE messages: Carlos and Victor write them here in P1 and F2.
- **Testing without a stand-in.** There is no prototype or stand-in device (decided 2026-10-13): the phone tests directly against Victor's firmware on the Waveshare ESP32-S3 e-Paper 3.97 board. `phone/tools/audio_reference/` keeps a Python reference encoder and golden test vectors, so the firmware can check its chunks byte for byte against what the phone expects before the two ever connect.

*State ownership.* The backend owns requests, revisions, proposals and execution status. The device enters a decision or outcome state (and starts the 1 s guard in REVIEW) only when the phone sends that screen. The device handles BOOT / RECOVERY, READY, WALLPAPER, PHONE / BLUETOOTH CHECK, PHONE DISCONNECTED, RECORDING, RECORD ADDITION, FINALIZE AUDIO and CRITICAL BATTERY locally. If the link stalls beyond the audio buffer, capture stops with FAILURE; audio is never silently dropped.

*Screen text.* Fixed-width font on the device. The backend sends pre-wrapped `screen_lines`, so what it checks is exactly what is shown. Panel: 800 x 480, 4 gray levels (Waveshare ESP32-S3 e-Paper 3.97). Budget per orientation (characters per line, lines per screen): _pending font choice (Victor, F1)_. The REVIEW screen must fit at least the full email address, the subject and 2 body lines; rotating scrolls the rest.

**Device state machine** (V1, 9 October 2026, plus team additions)

Inputs: hold (record while held), release (finish the voice segment), single press (select or approve), double press (cancel or dismiss), rotate up/down (move the highlight or scroll). Single press, double press and hold come from the side button; the rotary switch rotates and wakes. Only two numbers come from the spec: WALLPAPER after more than 1 minute of inactivity in READY, and CRITICAL BATTERY at 5% or less.

Request lifecycle, main path:
1. READY, hold: PHONE / BLUETOOTH CHECK, then RECORDING (needs Bluetooth and a ready phone service, otherwise PHONE DISCONNECTED).
2. Release: FINALIZE AUDIO until every chunk and the final marker are acknowledged, then PROCESSING.
3. PROCESSING (PROCESSING SLOW past the threshold). The latest result routes to CONFLICT-AMBIGUOUS, CONFLICT-SCHEDULE, REVIEW or READ-ONLY RESULT.
4. A hold in PROCESSING, PROCESSING SLOW, REVIEW or a conflict records an addition: the revision goes up, the old proposal is invalidated, and the whole request reruns.
5. REVIEW, single press (after the 1 s guard): approve the displayed revision. EXECUTING with a stable execution key, then SUCCESS, FAILURE or EXECUTION UNCERTAIN. An uncertain outcome is resolved by querying the same key (RECONCILE STATUS), never by sending again.
6. Double press before approval: CANCELLED, nothing sent. 60 s without interaction in REVIEW or a conflict: CANCELLED.
7. Battery at 5% or less: CRITICAL BATTERY from every state.

Every transition, copied from the team's Mermaid source (`ai-badge-state-machine.mmd`). Each "→" is one transition and its text is the trigger or guard. Not repeated per row: battery at or below 5% moves every state to CRITICAL BATTERY.

| State | Kind | Transitions |
| --- | --- | --- |
| BOOT / RECOVERY | Operational | Startup checks pass; no pending execution → READY • Recover a persisted pending execution → RECONCILE STATUS |
| READY | Ready or outcome | Inactivity > 1 minute → WALLPAPER • Hold begins a new request → PHONE / BLUETOOTH CHECK |
| WALLPAPER | Operational | Press or rotate wakes; consume wake input → READY |
| CRITICAL BATTERY | Exception | Battery <= 5%: inhibit new capture and execution → CRITICAL BATTERY • Battery > 5% and pending execution exists → RECONCILE STATUS • Battery > 5% and no pending execution → READY |
| PHONE / BLUETOOTH CHECK | Operational | Phone linked and capture service ready → RECORDING • Phone absent or service unavailable → PHONE DISCONNECTED • Double press: cancel → CANCELLED |
| PHONE DISCONNECTED | Exception | Notice acknowledged; retry from READY → READY • Double press: cancel → CANCELLED |
| RECORDING | Operational | Hold: stream bounded, numbered chunks → RECORDING • Release: close segment; send final marker → FINALIZE AUDIO • Link loss, capture error or buffer exhaustion → FAILURE • Double press: cancel → CANCELLED |
| FINALIZE AUDIO | Operational | All chunks and final marker acknowledged → PROCESSING • Delivery deadline or transport failure → FAILURE • Double press: cancel → CANCELLED |
| PROCESSING | Operational | Processing exceeds configured slow threshold → PROCESSING SLOW • STT/model error or processing deadline → FAILURE • Hold: add voice to this request → RECORD ADDITION • Latest result has multiple matching candidates → CONFLICT-AMBIGUOUS • Latest result has a scheduling conflict → CONFLICT-SCHEDULE • Latest result is an unambiguous proposed action → REVIEW • Latest result is a read-only answer → READ-ONLY RESULT • Double press: cancel → CANCELLED |
| PROCESSING SLOW | Operational | Progress resumes; latest revision remains active → PROCESSING • STT/model error or processing deadline → FAILURE • Hold: add voice to this request → RECORD ADDITION • Double press: cancel → CANCELLED |
| RECORD ADDITION | Operational | Release: finalize addition; rerun latest revision → FINALIZE AUDIO • Link loss, capture error or buffer exhaustion → FAILURE • Double press: cancel → CANCELLED |
| CONFLICT-AMBIGUOUS | User decision | Hold: answer clarification by voice → RECORD ADDITION • Rotate up/down: change highlighted candidate → CONFLICT-AMBIGUOUS • Single press: select candidate ID; increment revision → PROCESSING • Double press: cancel → CANCELLED |
| CONFLICT-SCHEDULE | User decision | Hold: change conflicting details by voice → RECORD ADDITION • Single press: explicitly continue despite conflict → REVIEW • Double press: cancel → CANCELLED |
| REVIEW | User decision | Hold: revise the proposal by voice → RECORD ADDITION • Rotate: scroll proposal details → REVIEW • Single press: approve displayed revision → EXECUTING • Double press: cancel → CANCELLED |
| READ-ONLY RESULT | Ready or outcome | Rotate: scroll result → READ-ONLY RESULT • Single press or result timeout → READY |
| EXECUTING | Operational | Authoritative success for execution key → SUCCESS • Authoritative rejection / confirmed no effect → FAILURE • Timeout, link loss, ambiguous response or restart → EXECUTION UNCERTAIN • Double press: stop waiting; effect may already exist → EXECUTION UNCERTAIN |
| EXECUTION UNCERTAIN | Exception | Single press or background status query → RECONCILE STATUS • Double press: dismiss; persist unresolved action → READY |
| RECONCILE STATUS | Operational | Confirmed applied → SUCCESS • Confirmed not applied / rejected → FAILURE • Status remains inconclusive → EXECUTION UNCERTAIN |
| SUCCESS | Ready or outcome | Acknowledgment or outcome timeout → READY |
| FAILURE | Exception | Acknowledgment or outcome timeout → READY |
| CANCELLED | Ready or outcome | Cancellation notice completes → READY |

Team additions (decided 2026-10-09, not in the Mermaid source):

| State | Added transition or rule |
| --- | --- |
| REVIEW | Single presses ignored for about 1 s after the screen finishes drawing (confirm guard) |
| REVIEW, CONFLICT-AMBIGUOUS, CONFLICT-SCHEDULE | 60 s after the last interaction → CANCELLED |
| PROCESSING | Latest result has no matching contact → CONFLICT-AMBIGUOUS with an empty candidate list ("hold to say the address") |
| PHONE DISCONNECTED, SUCCESS, FAILURE | Any input (press, double press, hold or rotate) counts as the acknowledgment |
| BOOT / RECOVERY, CRITICAL BATTERY | The pending execution is saved on the backend and its key in device flash |

Global rules from the spec (they take precedence over ordinary transitions):
1. Battery first: at 5% or less, stop capture and block new execution; a dispatched action's key stays saved as pending.
2. One request, many revisions: tag everything with request ID, segment ID and revision; only the latest non-cancelled revision is shown or executed.
3. Cancellation: a double press in any pre-execution state cancels locally at once, invalidates the request and signals upstream; late responses are ignored; a release after cancelling submits nothing.
4. Approval: only a single press in REVIEW dispatches a mutating action, bound to the displayed revision and proposal hash, with a stable execution key.
5. Phone and display: capture needs a ready phone service; link loss during capture or finalization fails the segment; e-ink updates are serialized and coalesced.
6. Returns: success, cancellation and acknowledged failures go to READY; an uncertain action can be dismissed but stays saved; only READY goes to WALLPAPER.
7. Input arbitration: tell single from double press before approving; a hold suppresses the click on release; candidates use stable IDs and a versioned list; battery, cancellation, addition and response events are handled atomically.

**Latency budget** (provisional: starting guesses, replace with measurements)

| Step | Owner | Target (s) | Measured (s) |
| --- | --- | --- | --- |
| Audio from device to phone over BLE | Victor | 0.5 | |
| Parakeet transcription on phone | Carlos | 0.5 | 0.19 to 0.32 (P0, 5 to 9 s clips, model already loaded) |
| Phone to backend | Carlos | 0.2 | |
| Nemotron tool loop | Orlando | 1.2 | |
| Google or Tavily call | Orlando | 0.6 | |
| Backend to phone to device | Carlos, Victor | 0.3 | |
| Final e-ink refresh | Victor | 1.5 | |
| **Total** | | **4.8** | |

Gesture timing starting values (Device, provisional): hold over about 500 ms; double press = two presses within about 400 ms.

## 3. Phases

Each phase ends with a gate. The gate must pass before the next phase in that track begins. Completed phases get marked **DONE** with results and gotchas recorded. Integration milestones (section 3.4) need all three tracks.

### 3.1 Backend (Orlando, `pipeline/`, branch `AI-Pipeline`)

#### Phase B0: Setup and model verification (DONE 2026-10-07)
- **Goal:** prove Nemotron 3 Nano tool calling works on Token Factory from this codebase, with secrets handled correctly.
- **Steps:** Python project (FastAPI, `openai` SDK, settings from environment); `.env.example`, `.env` gitignored; look up and record the exact model ID and base URL in CLAUDE.md; one plain completion and one tool-calling call with a toy tool; `/health`; per-hop timestamp logging.
- **Gate:** tool-calling script returns a well-formed tool call with valid JSON arguments in 5 of 5 runs; model ID and base URL recorded; no secrets tracked; Zero Data Retention confirmed on by Orlando.
- **Results (if DONE):**
  - Tool calling: 5 of 5 pass (`scripts/smoke.py --runs 5`). Command "block 3 to 4 tomorrow for the gym" gave `create_event` with title "Gym", start `2026-10-07T15:00:00-05:00`, 60 minutes, every run. Reasoning off, no reasoning text returned.
  - Latency, one model turn with tools: median 0.57 s, max 0.59 s (439 prompt tokens, 73 completion tokens). Plain completion 0.67 s. Measured from a laptop in Texas, not the Oracle server.
  - Model ID `nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B` and base URL recorded in CLAUDE.md, verified with `scripts/list_models.py`.
  - Reasoning switch verified: off gave 4 output tokens on a trivial prompt, on gave 96.
  - `/health` requires the API key (401 without, 200 with); one JSON timing line per hop with `request_id`. Offline tests: 3 of 3 pass (`.venv/bin/python -m pytest`).
  - No secrets tracked: `pipeline/.env` ignored, key values found in no committable file. Zero Data Retention confirmed on by Orlando.
  - Caveat: at temperature 0 the 5 runs are identical, so 5 of 5 proves well-formed output and a working setup, not accuracy. Accuracy is measured by the B2 eval set.
  - Budget note: 0.57 s is one model turn. A write command needs at least one turn, a lookup then act needs two or more, so the 1.2 s slot allows about two turns at this prompt size. Keep prompts small.

#### Phase B1: API contract and request state machine (DONE 2026-10-12)
- **Goal:** the request/response contract and the request lifecycle (revisions, approval, execution keys), built and tested before real tools exist.
- **Steps:** implement the API in section 2 (signed off 2026-10-12): endpoints including `touch`, `call_id` idempotency with 24 h retention and replay or reattach of streams, API keys with per-client scopes and rate limits, the error shape; Postgres schema and migrations for users, requests, revisions, segments, proposals, executions and history; latest revision wins and stale or cancelled responses are dropped; 60 s expiry in REVIEW and conflict states, reset by `touch`, additions, select and continue; approval bound to revision and proposal hash; short stable execution keys and the status endpoint; approve waits up to 5 s, then `executing`; a new request cancels an unapproved older one; saved pending executions; Server-Sent Events with `accepted`, `status`, `slow` and `result` events, `request_id` and `revision` on every event, `: ping` every 10 s.
- **Gate:** tests pass for: an addition invalidates the old proposal and only the latest revision is returned; an addition with a stale base revision is appended, not rejected; select, continue or approve with a stale revision returns `409 stale_revision`; late responses for cancelled requests are ignored; 60 s expiry cancels decision states and `touch` resets it; approval with a wrong proposal hash is rejected; a repeated approval, with the same or a new `call_id`, executes once; a repeated `call_id` replays a finished stream and reattaches to a running one; the same `call_id` with a different body returns `409 call_id_conflict`; the status endpoint returns success, failure, executing or execution_uncertain; cancel after approval returns `409 already_executing`; a new request cancels an unapproved older one; cancel leaves nothing behind; requests without the API key, or with the demo key in real mode, are rejected.
- **Results (if DONE):** gate passed, **tested against mocks** (fake processor, fake executors, fake clock) on a real Postgres 16. Real model in B2, real Google tools in B4.
  - Tests: 59 of 59 pass (`cd pipeline && .venv/bin/python -m pytest`, about 7 s). Lifecycle core 32, JSON endpoints 17, streams 10. Every gate item maps to at least one test; the timing sensitive stream tests passed 5 runs in a row.
  - Tests checked by breaking the code on purpose (15 breaks: user lock, expiry, execution key dedupe, stale result drop, demo scope, replay, retention, body check, rate limit, wait for running call, reattach, stream replay, slow event, refusal storage, abandoned call). All 15 are caught. Two tests were too weak at first and were fixed: the concurrent approval test never overlapped (pool opened with 1 connection), and the refused start test could not tell a replay from a second refusal.
  - Defense in depth: with the user lock removed on purpose, a second simultaneous approval fails on the execution key's primary key instead of executing twice. The database also enforces one open request per user (partial unique index) and one execution per request.
  - Real server (uvicorn, 1 worker, dev database, curl and httpx): a full flow (stream start, approve, cancel refused, status, second approve with a new `call_id` returning the same key, one execution row). Events arrive live, not buffered: `accepted` 0.01 s, `: ping` every 1.5 s (test setting), `slow` at 2.0 s, `result` at 5.0 s with a 5 s fake processor. Plain `\n` line endings, no `\r`.
  - Gate item "demo key in real mode is rejected": built as "a demo (`mock` scope) key can never reach real tools", since the API has no mode field; the key's scope decides. Tested with real tools switched on.
  - Contract additions, confirmed by Carlos on 2026-10-13 and written into section 2: `429 rate_limited` (per key rate limit), and `expired` also covers a request that is cancelled or finished (no separate `cancelled` code).
  - Local database: `cd pipeline && docker compose up -d` (Postgres 16, `127.0.0.1:5433` only), then `.venv/bin/python scripts/migrate.py` (and `--test`). Keys: `scripts/create_key.py --client NAME --scope real|mock`.

#### Phase B2: Agent loop with mock tools (PLANNED)
- **Goal:** Nemotron reliably picks the right tool with the right arguments, in English, against mock data.
- **Steps:** tool schemas for all ten tools; realistic mock calendar, inbox and contacts; system prompt with date, time, timezone, language, text budget and relevant memory; iteration cap on the loop; `screen_lines` built and length-checked in code against the configured budget; all-in-one email with recipient resolution and auto subject; several or zero matching contacts return CONFLICT-AMBIGUOUS candidates (an empty list for zero); schedule overlaps return CONFLICT-SCHEDULE; questions that change nothing return READ-ONLY RESULT; an addition reruns the whole request; English eval set of spoken-style commands (relative dates like "tomorrow at 3", ambiguous recipients, search then act, requests that must not trigger a write).
- **Gate:** eval results per category. **Provisional target: at least 90% correct tool and arguments overall, and 100% on "no write without a pending confirmation".** The 90% is not validated; replace it after the first run. Median model loop latency reported.
- **Results (if DONE):**

#### Phase B3: Memory (PLANNED)
- **Goal:** persistent contacts, preferences and recent actions that change what the assistant does.
- **Steps:** Postgres tables; `remember` and `recall`; inject only relevant memory into the prompt; endpoints to view, edit and delete memory for the phone app and demo site.
- **Gate:** "email Victor" resolves from memory; an edited or deleted memory changes the next answer; "move that meeting to 4" finds the right event from history.
- **Results (if DONE):**

#### Phase B4: Real integrations (PLANNED)
- **Goal:** the same loop against real Google services and Tavily through the dedicated demo account.
- **Steps:** OAuth in testing mode with the demo account as test user, tokens stored outside git; real Calendar, Gmail send and People "other contacts" behind the same interface; Tavily; mock or real switch per session.
- **Gate:** on the demo account, create an event, send an email to a team member, and one search then act request. Logs show Gmail was not called before confirmation, and declined or expired drafts left nothing in Gmail.
- **Results (if DONE):**

#### Phase B5: Deployment and latency (PLANNED)
- **Goal:** always-on backend on the Oracle ARM server, inside its share of the latency budget.
- **Steps:** deploy, HTTPS with a real certificate, Postgres with daily backups copied off the server, secrets in the server environment, per-hop timing summary; tune reasoning and prompt size.
- **Gate:** backend timings over at least 20 requests on the deployed server compared with the budget table; Phase B2 eval re-run on the deployed config shows no accuracy regression.
- **Results (if DONE):**
  - Progress 2026-10-14 (not DONE): early deploy so the phone can start P2. B1 backend in mock mode at `https://api.haloai-cov.io` (Oracle ARM, Chicago), deployed from main with `pipeline/scripts/deploy.sh`. Verified from the internet: Let's Encrypt certificate, 401 without the key, `/docs` and `/openapi.json` blocked (404), HTTP redirects to HTTPS, a full flow (stream start, approve, status) with `/health` in 142 ms from Texas. A reboot brought Postgres, the backend, Caddy and the firewall rules back on their own. Still open for the gate: daily backups copied off the server, 20 request timings, the B2 eval on the server. Not verified yet: live delivery of slow streams through Caddy (the fake processor answers in milliseconds); checked when B2 runs there.

#### Phase B6: Demo site support (PLANNED)
- **Goal:** judges can try HaloAI without hardware or a Google login.
- **Steps:** sample data mode as default; endpoints for push to talk or typed input, a mock e-ink screen with the same text budget, live memory and action panels.
- **Gate:** a person with no context completes a calendar flow and an email flow on the demo site.
- **Results (if DONE):**

#### Phase B7: Spanish and bilingual (PLANNED, only after English works end to end)
- **Goal:** the same flows in Spanish, then in mixed English and Spanish.
- **Steps:** add Spanish and mixed commands to the eval set; replies and `screen_lines` in the user's language; Spanish date phrases ("mañana a las 3"); review the phone's Spanish transcription quality.
- **Gate:** Spanish results reported per category next to English. **Provisional target: Spanish within a few points of English, no English regression.**
- **Results (if DONE):**

### 3.2 Phone (Carlos, `phone/`, branch `Phone-App`)

#### Phase P0: Parakeet on the phone (DONE, 2026-10-07)
- **Goal:** prove English speech to text runs fully on Carlos's phone, fast enough.
- **Steps:** Flutter project for Android; Parakeet v3 on device; transcribe at least 5 English test WAV files (16 kHz mono) of real commands such as "send an email to Victor saying the meeting moved to Thursday" and "block 3 to 4 tomorrow for the gym".
- **Gate:** transcription runs with no network; per-clip text, errors and time recorded here.
- **Results (if DONE):** gate passed.
  - **Setup:** Samsung Galaxy Z Flip7 (SM-F766U, Exynos 2500, Android 17). sherpa_onnx 1.13.8 (Flutter), model `sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8`, 4 threads, CPU. Release build, whose manifest has no INTERNET permission, run with airplane mode on (verified: no active network).
  - **Clips:** 5 commands recorded by Carlos on his phone mic (not the device mic), converted to 16 kHz mono WAV.
  - **Model load:** 1.74 s, once per app start. It must stay loaded between commands, not reload per command.
  - **Accuracy:** 7 word errors in 55 reference words (12.7% word error rate, lowercase, punctuation removed, "four" counted as "4"). 2 of 5 clips have errors that change the meaning.
  - **Speed:** 186 to 320 ms per clip, about 0.04 times the audio length. Inside the 0.5 s budget. Warm runs within 7 ms of cold runs.

  | Clip | Audio (s) | Decode (ms) | Said | Transcribed | Errors |
  | --- | --- | --- | --- | --- | --- |
  | block_3_4_tmrw_gym | 6.0 | 236 | Block 3 p.m. to 4 p.m. tomorrow for the gym. | Block 3 p.m. to four p.m. tomorrow for the gym. | none ("four" is formatting only) |
  | calendar_12_2_meeting | 8.6 | 319 | Add on my calendar on October 5th, a meeting between 12 p.m. and 2 p.m. | Add to my calendar on October 5th, a meeting between 2 p.m. to 2 p.m. | 3, **12 heard as 2 (wrong time)** |
  | delete_meetings_thursday | 4.7 | 186 | Delete all my meetings on Thursday. | Delete all my meetings on Thursday. | none |
  | email_meeting_moved | 5.4 | 214 | Send an email to Victor saying the meeting got moved to Thursday. | Send an email to Victor saying the meeting moved to Thursday. | 1, harmless |
  | email_tomai_meeting_sick | 6.8 | 270 | Email Tomai saying I cannot attend the meeting because I am sick. | Email to my saying I cannot attend a meeting because I am sick. | 3, **name "Tomai" lost** |

  - **What it means:** speed is not a problem. Accuracy on uncommon names and on number ranges is the real risk, and those are exactly the fields that matter for email and calendar. Mitigations: the confirm screen must always show the resolved recipient and times; the backend must ask back when a recipient doesn't resolve ("Email to my" should trigger a question, not a guess). Possible later improvement: sherpa-onnx hotwords or contextual biasing with names from the user's contacts (not tested yet).
  - **Not yet tested:** the board's onboard mic (re-test in P1 when the board arrives); noisy environments; phone locked (P3).

#### Phase P1: Bluetooth link and audio from the device (PLANNED)
- **Goal:** receive audio from the device over BLE and transcribe it.
- **Steps:** implement the BLE service in section 2 with Victor (write the exact JSON field names there); pairing and reconnect handling, resyncing from `hello`; store each Audio chunk, check its CRC and send cumulative acknowledgments on Audio Ack (with `resend_from` for gaps), drop duplicates by identity, reassemble by sequence number, acknowledge the final marker, decode to 16 kHz mono PCM from each chunk's ADPCM state; feed Parakeet once the final marker confirms the segment is complete; map device IDs to API UUIDs and `sid` to revision and proposal hash; test against Victor's firmware on the Waveshare board. Until his F2 build streams audio, the phone side is proven with unit tests and the golden vectors in `phone/tools/audio_reference/`.
- **Gate:** a spoken command on the real device comes out as correct text on the phone; BLE transfer time measured against the budget.
- **Results (if DONE):**
  - Progress 2026-10-13 (not DONE, gate needs the real board with Victor's firmware): phone side built in `phone/lib/ble/` (chunk parser with CRC, IMA ADPCM decoder, segment assembler with cumulative acks and `resend_from`, ID and `sid` maps, JSON messages and fragments, BLE link over flutter_blue_plus, device session). **Tested on the Mac only** with a fake transport: 37 of 37 tests, including byte for byte agreement with a Python reference encoder (golden vectors for the firmware) and a real P0 clip through the full audio path with lost chunks. Deliberate breaks (nibble order, duplicate check, final ack) were each caught. Not yet run on the phone against a device. Field names and UUIDs are a draft for Victor in `phone/docs/ble_fields_draft.md`.
  - On the phone 2026-10-13 (Z Flip7, release build): loopback of the 5 P0 clips through the full audio path (encode, chunks with every 9th lost on purpose, receiver, decode) in 9 to 22 ms per clip; all lost chunks (25 to 47 per clip) recovered by `resend_from`. 4 of 5 transcripts identical to the original WAV's. The fifth is the P0 "12 p.m." clip: the original still reads "2 p.m.", the ADPCM version read "12 p.m." (correct). ADPCM's small changes can flip a borderline word either way; REVIEW stays the safety net. No radio involved yet.

#### Phase P2: Backend client (PLANNED)
- **Goal:** the phone talks to the backend with the agreed contract.
- **Steps:** send text with date, time, IANA timezone, language and orientation; send the API key and a new `call_id` per POST (reused on retry) plus the request ID and revision; read the Server-Sent Events streams with a 25 s read timeout and retry on the same `call_id`; forward `screen_lines` and status to the device; forward approve, additions, candidate selection, continue, cancel, `touch` (from `activity`), acknowledgments and execution status queries; poll the status endpoint after an `executing` approve; drop streams and events older than the latest `accepted`; handle empty transcripts without calling the backend (section 2).
- **Gate:** against the backend in mock mode, a calendar flow and an email flow (including an addition, a candidate selection and a cancel) complete from the phone.
- **Results (if DONE):**

#### Phase P3: Works locked in a pocket (PLANNED)
- **Goal:** the app keeps working with the screen locked.
- **Steps:** Android foreground service for Bluetooth, transcription and network; battery optimization exemption if needed.
- **Gate:** with the phone locked for at least 10 minutes, a command from the device still completes end to end.
- **Results (if DONE):**

#### Phase P4: App features (PLANNED)
- **Goal:** settings and memory control.
- **Steps:** orientation setting; wallpaper picker that resizes and dithers to the panel's resolution and color depth, then sends it; Google account link; view, edit and delete memory; full text of long drafts.
- **Gate:** each feature works against the real device or backend; a wallpaper set on the phone shows on the device in WALLPAPER.
- **Results (if DONE):**

#### Phase P5: Spanish and bilingual transcription (PLANNED, only after English works end to end)
- **Goal:** measure and enable Spanish and mixed English and Spanish.
- **Steps:** test clips in Spanish and mixed sentences; report accuracy honestly.
- **Gate:** results recorded; team decides whether to demo mixed sentences or one language at a time.
- **Results (if DONE):**

#### Phase P6: iOS (STRETCH, only if Android is done early)
- **Goal:** the same app on iPhone from the Flutter codebase.
- **Gate:** first test is whether audio arrives and is transcribed with the iPhone locked.
- **Results (if DONE):**

### 3.3 Device (Victor, `firmware/`, branch `Hardware`)

#### Phase F0: Gestures and state machine (PLANNED)
- **Goal:** the V1 state machine in section 2 (with the team additions), implemented and tested.
- **Steps:** propose Arduino core vs ESP-IDF with tradeoffs and get a decision; hardware-independent gesture detector and state machine (testable on a computer); thin ESP32 layer that reads the button and logs every gesture and transition over serial. No BLE, no screen yet. Timing constants in one config file.
- **Gate:** tests cover every transition in the V1 table and the team additions, including the 1 s confirm guard in REVIEW, the 60 s expiry in decision states, any-input acknowledgment, WALLPAPER only from READY after more than 1 minute, and CRITICAL BATTERY from every state; serial log on real hardware shows correct single, double, hold and rotate detection.
- **Results (if DONE):**

#### Phase F1: Hardware choice and e-ink screen (PLANNED)
- **Goal:** the final board driving real screens.
- **Steps:** build directly on the Waveshare ESP32-S3 e-Paper 3.97 board (in hand; no prototype stage); add the rest of the final hardware from the materials inventory (a flat 3.7 V LiPo that fits a body of roughly credit card size, side button, vibration motor with driver; see CLAUDE.md Environment); render each state's screen; partial refresh for the status bar, full refresh for final screens; wallpaper stored in flash and shown in WALLPAPER; report the text budget per orientation (800 x 480 panel) to the team.
- **Gate:** every state's screen renders on the real panel; full and partial refresh times measured; text budget written into section 2.
- **Results (if DONE):**

#### Phase F2: Microphone and BLE audio (PLANNED)
- **Goal:** recorded speech reaches the phone as streamed IMA ADPCM.
- **Steps:** capture 16 kHz mono from the board's onboard microphone (ES8311 audio codec) while the button is held; encode IMA ADPCM; stream raw-byte Audio chunks in the section 2 layout (ADPCM state and CRC in every chunk) from a 2 s ring buffer during the hold and free them on cumulative Audio Ack, resending from `resend_from`; on release send the final marker and wait for its acknowledgment (FINALIZE AUDIO); stop capture with FAILURE if the link stalls beyond the buffer; send `hello` on every connect, `record_start` and `record_end` as JSON Events, and rate-limited `activity` on knob rotation; preferred MTU 256, fail the connection below 179, 2M PHY if supported; write the BLE JSON field names into section 2 with Carlos.
- **Gate:** Carlos's phone transcribes audio recorded on the device correctly; transfer time measured against the budget.
- **Results (if DONE):**

#### Phase F3: Full device flow (PLANNED)
- **Goal:** the device runs the real flow with the phone.
- **Steps:** screens, gestures and rotation wired to BLE messages; 1 s confirm guard; 60 s expiry; WALLPAPER; CRITICAL BATTERY; BOOT / RECOVERY with the execution key saved in flash.
- **Gate:** calendar and email flows (approve, addition, candidate selection, continue, cancel, expiry, uncertain execution and reconcile) work from the device with the phone and backend.
- **Results (if DONE):**

#### Phase F4: Power and battery (PLANNED)
- **Goal:** usable battery life and fast wake.
- **Steps:** compare light sleep (BLE link kept) with deep sleep: wake time, reconnect time, battery cost; choose battery size and a battery life target; safe charging and protection: confirm what the board's TG28 power management chip covers and whether the LiPo pack has its own protection circuit.
- **Gate:** measured wake plus reconnect time and battery life written here; charging circuit reviewed for LiPo safety.
- **Results (if DONE):**

### 3.4 Integration milestones (all three)

- **I1: Text round trip without the device.** Phone sends typed or transcribed text to the backend and shows the reply. Needs B1, P2.
- **I2: First end to end command.** Hold the button, say a calendar command, see REVIEW, approve with a single press, event appears in the calendar (mock or real). Needs B2, P2, F3.
- **I3: Email flow with an addition and a cancel** on real hardware. Needs B4, P3, F3.
- **I4: Latency measured end to end** on real hardware against the 5 s budget, with per-hop timings filled into section 2.
- **I5: Submission.** See section 5.

Target dates for I1 to I4: _to be set by the team_.

## 4. Open Risks

- Text budget numbers wait on the font choice for the 800 x 480 panel (F1). The backend and the phone app keep them as configuration until then.
- The Waveshare board is in hand, but the flat LiPo, side buttons and vibration motors are still Planned or in the cart (materials inventory, 2026-10-09). Shipping time comes out of the October 30 deadline: order them now. With no prototype stage, nothing else can stand in while they ship.
- The device firmware is the critical path (as of 2026-10-13, 17 days before the deadline): F0 has not started and the framework is not chosen. The P1 gate, I2, I3 and the demo video all need the board running the firmware. With no prototype stage there is no fallback device, so firmware slips move the whole schedule.
- Bluetooth throughput for about 70 kbps of audio chunks (64 kbps ADPCM plus headers) plus JSON messages is unmeasured. Victor measures it early in F2; if it falls short, the fallback is record then send.
- Prompt injection through tool results (a web page or email telling the model to act). Mitigation: tool output treated as data; the confirmation gate stays the backstop for every write.
- Spoken email addresses transcribe badly. Mitigation: contacts first; dictated addresses always shown in full in REVIEW.
- Android killing background work. Mitigation: foreground service, tested in P3.
- E-ink refresh time eating the budget. Mitigation: partial refresh for status, one full refresh per final screen.
- Accidental presses in a pocket. Mitigation: 1 s confirm guard in REVIEW; send is never a default.
- The V1 API, BLE characteristics and audio byte layout were signed off on 2026-10-12. Still open: the exact JSON field names for the BLE messages (Carlos and Victor, P1 and F2) and the ring buffer size, which is provisional until F2 testing.
- Google OAuth testing mode tokens expire every 7 days. Re-authorize before the video and before judging.
- Oracle may stop the Always Free server if it looks idle for 7 days (server created 2026-10-14). Check it before the video and before judging, or move the account to Pay As You Go.
- Token Factory credits are finite. Log token usage per request.
- Spanish and mixed-language quality unknown. Deferred to B7 and P5; demo mixed language only if testing supports it.

## 5. Submission Checklist (due October 30, 2026, 10:00 AM PDT, 12:00 PM in Edinburg, TX)

- [ ] Make the repo public before submitting (it is private until then; MIT license file already in the repo), with a README with setup steps for backend, phone app and firmware
- [ ] Demo site live in sample data mode
- [ ] YouTube video under 3 minutes showing the real device, the demo site and one search then act moment (a bilingual moment only if Spanish is ready)
- [ ] Written description of features, naming Nemotron, Parakeet, Nebius Token Factory and Tavily
- [ ] Track selected: Personal AI
- [ ] Google demo account re-authorized before judging, December 1 to 15
