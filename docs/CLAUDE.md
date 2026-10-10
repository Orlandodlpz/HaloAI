# CLAUDE.md: HaloAI

Project context and conventions for the whole team. Read this first in every session, whatever part of the project you work on.
**Last Updated:** 2026-10-10

## Interaction Mode: Always On

**Use calibrated honesty for all work on this project, at all times, no exceptions.** Agree when the approach is right, push back plainly when it isn't, and don't manufacture agreement or hedge to avoid friction. This applies to code review, architecture decisions, hardware choices, data quality, and any claims made in docs or commit messages, including anything already written in this file. If something in this project's plan is flawed, say so directly rather than deferring to what's documented.

## Project Overview

HaloAI (built by team COV: Carlos, Orlando, Victor) is a roughly credit card sized voice assistant that clips to your pants. The user holds a button, speaks a command such as "block 3 to 4 tomorrow for the gym" or "send an email to Victor saying the meeting moved to Thursday", reviews the result on a small e-ink screen, and confirms with a button press.

How it works: the **device** (Waveshare ESP32-S3 e-paper board with microphone, a side button, the board's rotary button, a vibration motor, battery) sends audio over Bluetooth Low Energy to an **Android phone app**, which turns speech into text with NVIDIA Parakeet v3 on the phone and sends **text only** over HTTPS (Wi-Fi or cellular) to the **cloud backend**. The backend uses NVIDIA Nemotron 3 Nano on Nebius Token Factory to turn the text into a calendar or email action, holds it as a draft until the user confirms on the device, then executes it.

Entry: Nebius x NVIDIA Global AI Hackathon, **Personal AI track**. Submission deadline: **October 30, 2026, 10:00 AM PDT (12:00 PM in Edinburg, TX)**.

**Team and ownership:**

| Area | Owner | Folder | Branch |
| --- | --- | --- | --- |
| AI pipeline (cloud backend, demo site API) | Orlando | `pipeline/` | `AI-Pipeline` |
| Phone app and speech to text | Carlos | `phone/` | `Phone-App` |
| Device hardware and firmware | Victor | `firmware/` | `Hardware` |
| Shared docs | Everyone (own sections only) | `docs/` | each person's branch |

**Scope:** calendar and email only for now. The device is meant to handle more later (notes and others); each new action is a new backend tool.

**Language:** English first. Every part is built and tested in English until the whole flow works end to end. Spanish and mixed English and Spanish come after that.

## Hard Constraints (never violate, never soften)

**System wide**
- The only LLM is an NVIDIA open model on Nebius Token Factory: Nemotron 3 Nano. No Claude API, no OpenAI models, no other providers. Hackathon eligibility depends on this.
- Nothing irreversible happens without approval on the device: only a single press in REVIEW dispatches sending email or creating, moving or deleting events. Read-only results never execute.
- Raw audio never leaves the phone. The cloud receives text only.
- Bluetooth Low Energy only between device and phone. Never Bluetooth Classic: iPhones block it for uncertified accessories, which would rule out iOS later.
- The repo is public under the MIT license. No secrets in git (API keys, OAuth secrets, tokens, Wi-Fi passwords). Use `.env` files that are gitignored, with `.env.example` listing names only.

**Backend**
- Gmail is never contacted before approval; drafts live on our server only.
- One request at a time, with revisions. Every chunk, transcript, proposal and response carries the request ID, segment ID and revision; only the latest non-cancelled revision is ever shown or executed, and late responses for cancelled work are ignored.
- Approval is exactly once: it binds to the displayed revision and proposal hash and uses a stable execution key, so retries and status queries never create a duplicate effect.
- A proposal in REVIEW, CONFLICT-AMBIGUOUS or CONFLICT-SCHEDULE is cancelled 60 s after the last interaction.
- `screen_lines` must fit the e-ink text budget, taken from configuration, never hardcoded.
- Every API request requires the API key. The backend is public and can send email from the demo account.
- Tool results (web pages, email bodies, event text) are untrusted data, never instructions to the model.
- Zero Data Retention must be enabled on the Nebius Token Factory account whose API key the backend uses.

**Device**
- The 1 second confirm guard (single presses ignored for about 1 s after a REVIEW screen finishes drawing) and the gesture timings must never be bypassable by any code path that approves an action.
- Lithium battery safety: proper charging and protection circuit. Flag any design that charges or discharges a LiPo without one.
- The device never talks to the cloud directly; only to the phone.

## Repo and Git Workflow

- Each person works only on their own branch, created from `main`, and only in their own folder.
- Never commit to, push to, or merge into `main` from a working branch. Merges into `main` happen after review by the team, done by Orlando as repo owner.
- Push your branch to origin only when its owner says so.
- Shared docs: edit **only your own sections** of `docs/CLAUDE.md` and `docs/BRIEFING.md` (marked Backend, Phone or Device). Shared sections (overview, system-wide constraints, contracts, the state machine) change only after the team agrees, so three branches don't edit the same lines.
- Keep `.gitignore` files inside your own folder. Orlando owns the root README.
- Before every commit, run `git status` and confirm no secrets are staged. Stage files by explicit path.

## Environment

**Backend (Orlando)**
- Host: Oracle Cloud Always Free **ARM (aarch64)** server, always on. Check every Python dependency has an aarch64 wheel or builds there.
- Stack: Python 3.11 or newer, FastAPI, the `openai` Python SDK pointed at the Nebius Token Factory OpenAI-compatible endpoint, Postgres on the same server.
- Model: Nemotron 3 Nano, sparse mixture of experts (about 31.6B total parameters, about 3.6B active per token), tool calling, reasoning on/off switch. **Do not hardcode the model ID from memory.**
  - Model ID (verified 2026-10-06 against `GET /v1/models`): `nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B`
  - Base URL (from Nebius API docs, confirmed by a live call): `https://api.tokenfactory.nebius.com/v1/`
  - Verify: `cd pipeline && .venv/bin/python scripts/list_models.py`
  - Reasoning switch: `extra_body={"chat_template_kwargs": {"enable_thinking": False}}`. Token Factory passes it through (verified). With reasoning off, use temperature 0 (model card: greedy).
- Local dev: Python 3.13 venv in `pipeline/.venv` (`python3.13 -m venv .venv && .venv/bin/pip install -e '.[dev]'`). `openai` SDK is major version 3.
- External services: Google Calendar, Gmail (send), Google People ("other contacts"), Tavily web search. One dedicated demo Google account; the OAuth app stays in Google's testing mode.
- Credits: $25 Token Factory per member from the hackathon code, plus $25 Token Factory and $25 Tavily from the Nebius Builders Program. Nebius AI Cloud servers are not covered, which is why hosting is on Oracle.

**Phone (Carlos)**
- Flutter, Android first. iOS is a stretch goal before October 30 from the same codebase; building for iOS needs a Mac with Xcode.
- Test and demo phone: Carlos's Android phone. All phone timing numbers are measured on it.
- Speech to text: Parakeet v3 on the phone (sherpa-onnx is the usual route), running inside an Android foreground service so it keeps working with the phone locked.

**Device (Victor)**
- Hardware source of truth: the team's materials inventory, `AI_Badge_Hackathon_Materials_Updated.pdf` (2026-10-09). Status words below are copied from it: "Planned" and "Cart" mean not confirmed purchased. Board details marked (Waveshare) come from the vendor's spec page.
- Final board (Planned, 3 units target): Waveshare ESP32-S3 e-Paper 3.97 in. Integrated ESP32-S3 with Wi-Fi and BLE, 800 x 480 e-paper, onboard microphone, battery management and buttons. (Waveshare: 4 gray levels; ES8311 audio codec; TG28 power management chip with charging; MX1.25 battery header; PWR, BOOT and a rotary button with up, down and press; refresh full about 3.5 s, partial about 0.6 s, not yet measured by us.)
- Body: roughly credit card footprint (85.6 x 54 mm) and as thin as practical (thin enclosure: Future). The panel's active area alone is 86.40 x 51.84 mm (Waveshare), so the body will end up somewhat larger than a card.
- Inputs: 1 to 2 physical side buttons per badge (Planned) for press, double press and hold, prototyped with the tactile push button kit (Cart); the board's rotary button for rotate and wake.
- Feedback: the screen plus a vibration motor with driver, 1 per badge (Planned), prototyped with 3 V coin motors (Cart). No speaker: the team removed speaker output, so the board's speaker header and amplifier stay unused.
- Battery: flat 3.7 V LiPo, 1 per badge (Planned), capacity reduced if needed for thickness; prototyped with 3.7 V 1100 mAh packs with a 1.25 mm plug (Cart).
- In hand now: Raspberry Pi (temporary shared development computer over SSH and Tailscale) and a temporary e-ink display for prototype UI and low-power tests. The text budget and refresh times come from the final board, not the temporary display.
- Support materials (Recommended or Optional): transistor kit for the motor driver, resistor and diode kit, 3 breadboards, jumper wires, USB-C data cables, microSD cards, multimeter.
- Removed or replaced: XIAO ESP32-S3 boards and INMP441 microphones (the final board has both built in), speaker amplifier and speakers (speaker output removed), IR transmitter and receiver (feature dropped), round, AMOLED and IPS display boards (e-ink chosen for idle power).
- Firmware framework (Arduino core or ESP-IDF): _not chosen yet_.

## Locked Decisions

Decisions already made. Do not re-litigate unless new evidence is presented, and if re-opened, say so explicitly.

**System wide**
- **Device state machine: V1 (9 October 2026).** The full state and transition table is in BRIEFING.md section 2, "Device state machine". 21 states. Inputs: hold (record while held), release (finish the voice segment), single press (select or approve), double press (cancel or dismiss), rotate up/down (move the highlight or scroll). Single press, double press and hold come from the side button; the rotary switch rotates and wakes.
- Only a single press in REVIEW runs an action that changes anything. A double press cancels any pre-execution state; during EXECUTING it only stops waiting (EXECUTION UNCERTAIN), because the action may already have happened.
- A hold in PROCESSING, PROCESSING SLOW, REVIEW or a conflict state records an addition to the same request (RECORD ADDITION) and raises its revision.
- READY goes to WALLPAPER after more than 1 minute of inactivity; a press or rotation wakes it and that input is consumed. Active work never times out into WALLPAPER.
- CRITICAL BATTERY: entered from every state at 5% or less, left only above 5% (as written in the spec). Pending executions are reconciled before READY.
- **Team additions to the V1 spec (decided 2026-10-09):**
  - 1 s confirm guard: single presses are ignored for about 1 s after a REVIEW screen finishes drawing.
  - 60 s expiry: REVIEW, CONFLICT-AMBIGUOUS and CONFLICT-SCHEDULE go to CANCELLED 60 s after the last interaction.
  - Any input (press, double press, hold or rotate) acknowledges PHONE DISCONNECTED, SUCCESS and FAILURE.
  - A contact with no match goes to CONFLICT-AMBIGUOUS with an empty candidate list and a "hold to say the address" prompt.
  - A pending execution is saved on the backend (owner of the execution key and its status) and its key in device flash, so BOOT / RECOVERY and CRITICAL BATTERY can reconcile it.
  - Timeouts the spec leaves open (slow threshold, processing and delivery deadlines, result and outcome screen timeouts) are config values, provisional until measured on hardware.
  - Haptic feedback patterns are future work, only if the MVP is done early.
- **Email is all in one:** the user says recipient and content in one recording; REVIEW shows the full resolved address and the email. Several matching contacts, or none, go to CONFLICT-AMBIGUOUS instead of guessing.
- Status updates go to the screen only for meaningful steps ("Email to Victor", "Searching the web", "Ready to confirm").
- Every request to the backend carries the phone's current date and time, IANA timezone (for example America/Chicago, never a fixed offset), detected language and screen orientation.
- Target: about 5 s from button release to final screen for a simple command (provisional split in BRIEFING.md).

**Backend**
- Plain tool-calling loop against the OpenAI-compatible endpoint. No agent framework. No separate intent classifier (Jev and Laya were evaluated and rejected; picking a tool already classifies the request).
- Python + FastAPI; Postgres on the Oracle server; Oracle Cloud Always Free ARM hosting.
- Tools behind one interface with **mock** (sample data) and **real** implementations: `list_events`, `create_event`, `update_event`, `delete_event`, `lookup_contact`, `draft_email`, `send_email`, `web_search`, `remember`, `recall`. Confirmation required for `create_event`, `update_event`, `delete_event`, `send_email`.
- Tavily web search is used as search then act (results feed a calendar or email action).
- Previous recipients come from Google People "other contacts", not from reading Gmail.
- Memory stores contacts, preferences and recent actions; the user can view, edit and delete all of it.
- Subject is generated from the email body unless the user says one.
- Reasoning off by default; on only for multi-step requests.
- Demo site main mode: "Try it with sample data" (mock tools, no Google login).

**Phone**
- Flutter, Android first; Carlos's phone for testing and the demo.
- Parakeet v3 on the phone. English first.
- Forwards device events to the backend: approve, addition (transcribed text of a new recording), candidate selection, continue despite a conflict, cancel, and execution status queries.
- App features: Bluetooth pairing and status; orientation setting; wallpaper picker that resizes and dithers to the panel before sending; Google account link (sign-in happens on the backend); view, edit and delete memory; full text of long drafts.

**Device**
- Wallpaper set from the phone, stored in flash, shown in the WALLPAPER state.
- E-ink updates are serialized and coalesced (no redraw per audio chunk); inputs update the logical state immediately even while a refresh is running.
- Partial refresh for a small status bar; full refresh only for the final screen of each step.
- Orientation (landscape or portrait) is a setting sent from the phone.
- Gesture timings are config constants (starting values in BRIEFING.md).

## Contracts

Decided 2026-10-06. Full details in BRIEFING.md section 2. Changes need sign-off from both owners.

| Contract | Owners | Decision |
| --- | --- | --- |
| Message format, everywhere | Everyone | JSON for every message, with one exception: the Bluetooth audio channel carries raw bytes |
| Phone to cloud API | Orlando, Carlos | REST endpoints under `/v1/`, JSON bodies, API key in the `Authorization` header. Revised for the V1 state machine (request ID, revision, `call_id` for idempotent retries, proposal hash, execution key, status endpoint): **PROPOSED 2026-10-09, Orlando and Carlos sign off at the start of B1** |
| Status updates to the phone | Orlando, Carlos | Server-Sent Events streamed on the start, addition and select responses: `status` events, an optional `slow` event, then one `result` event. Approve, continue, cancel and status queries are plain JSON requests |
| Audio from device to phone | Carlos, Victor | Raw bytes (not JSON), streamed in numbered chunks while the button is held. Each chunk carries request ID, revision, segment ID, sequence number, format, byte count and an integrity check; the phone acknowledges each stored chunk; a final marker closes the segment. Codec: IMA ADPCM 16 kHz mono for now, tunable. Byte layout: **PROPOSED, Carlos and Victor** |
| Phone to device Bluetooth messages | Carlos, Victor | One custom BLE service with six characteristics: Audio (raw bytes), Audio Ack, Events, Screen, Wallpaper, Settings (JSON). Audio Ack and the V1 event and screen names: **PROPOSED 2026-10-09** |
| Screen text | Victor, for everyone | Fixed-width font on the device; backend sends pre-wrapped `screen_lines`. Panel: 800 x 480. Budget numbers (characters per line, lines per screen, per orientation) **still pending the font choice (Victor, F1)** |
| Source of truth for state | Everyone | Backend owns requests, revisions, proposals and execution status. The device enters a decision or outcome state only when the phone sends that screen. The device runs BOOT / RECOVERY, READY, WALLPAPER, PHONE / BLUETOOTH CHECK, PHONE DISCONNECTED, RECORDING, RECORD ADDITION, FINALIZE AUDIO and CRITICAL BATTERY on its own |

## Verification Rules

- Every configuration change ships with its verification command.
- Every phase ends with its gate check (BRIEFING.md) before the next phase starts.
- Every hop logs timestamps so latency can be attributed (device, phone and backend each log their own hops).
- Never claim something works without running it on the real target (real phone, real device, real service), or saying explicitly that only a mock or simulator was tested.
- Backend: any change to the system prompt, tool schemas or model settings is re-run against the eval set and compared with the previous run.
- Backend: request tests cover latest revision wins and stale responses are dropped, late responses for cancelled requests are ignored, 60 s expiry in decision states, approval only for the displayed revision and proposal hash, a repeated approval or status query executes once, and a repeated `call_id` returns the original result.
- Device: state machine tests cover every transition in the V1 table (BRIEFING.md section 2) plus the team additions, and the battery override from every state.

## Current Status

**Backend (Orlando):** Phase B0 DONE (2026-10-07): tool calling 5 of 5, median 0.57 s per model turn, Zero Data Retention on. Next: B1, once Carlos signs off on the PROPOSED V1 API. Text budget in config is a PROVISIONAL placeholder until Victor's F1 numbers.
**Phone (Carlos):** Phase P0 not started. Waiting on: text budget numbers (for the wallpaper size and screen preview).
**Device (Victor):** Phase F0 not started. Hardware target set in the materials inventory (final board Planned, not yet in hand). Waiting on: framework choice.

**Known gotchas discovered so far:**
- Nebius Token Factory stores prompts and outputs by default (for speculative decoding, not training). Zero Data Retention must be switched on per account.
- Google OAuth testing mode refresh tokens expire after 7 days. Re-authorize before recording the video and before judging (December 1 to 15, 2026).
- US daylight saving time ends November 1, 2026. Use timezone-aware datetimes with IANA zones everywhere.
- Raw 16-bit audio at 16 kHz is 256 kbps, more than ESP32 BLE reliably carries. ADPCM brings it to 64 kbps. Base64 inside JSON would add about a third, which is why audio is raw bytes.
- If nginx sits in front of FastAPI, turn off response buffering on the streaming routes (`/v1/requests` and its additions and select calls), or Server-Sent Events arrive all at once at the end.
- A single press can only be acted on after the double press window closes, which adds that window (about 400 ms) to every confirm.
- Free Apple developer accounts expire installed test builds after 7 days (matters only if the iOS stretch goal happens).
- The V1 state machine spec is a reconstruction: its BOOT recovery, transport failure policy, pending-action persistence and wake-input consumption rules were proposals. The team confirmed persistence (backend plus device flash) on 2026-10-09; the rest are still proposals.
- Backend: third party model listings give the Nemotron 3 Nano ID without the `NVIDIA-` part; the real ID is `nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B`. The Omni variant is dedicated endpoint only and not visible to a pay per token key.
- Backend: with reasoning on, Nemotron returns its thinking in a `reasoning` field on the message (not `reasoning_content`), and `usage` has no reasoning token breakdown. On a trivial prompt reasoning took 96 output tokens versus 4 with it off.
- Device: drive the vibration motor through a transistor, never straight from a GPIO pin, with a flyback diode across the motor. The transistor kit on the materials list is bipolar (TO-92 NPN/PNP), not MOSFET; pick a suitable driver transistor from it.
- Device: before plugging a LiPo into the board's MX1.25 header, check the connector type and the red/black polarity with the multimeter. 1.25 mm plugs from different vendors look alike but do not share a polarity standard.
- Device: use data capable USB-C cables. Charge only cables power the board but cannot flash it.
