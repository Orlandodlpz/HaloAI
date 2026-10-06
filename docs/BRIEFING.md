# BRIEFING.md: HaloAI Execution Plan

**Last Updated:** 2026-10-06
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
- **Device** produces audio and button signals (confirm, fix with a new recording, decline, cancel); consumes screen text, status updates, wallpaper and orientation. Owns gesture detection, the 1 s confirm guard, auto-off and e-ink rendering.
- **Phone** turns audio into text with Parakeet v3, adds date, time, IANA timezone, language and orientation, and talks to the backend. Relays replies and status to the device and signals to the backend. Never sends audio to the cloud.
- **Backend** runs the Nemotron tool-calling loop, holds pending actions, executes confirmed actions, stores memory and serves the demo site.

**Contracts** (see CLAUDE.md, "Contracts Not Yet Agreed"). When one is agreed, write it here.
- Phone to cloud JSON: _not written yet_. Must cover at least: a user or session id, command text, current time with timezone, language, orientation, and signals confirm / fix (with text) / decline / cancel keyed by a pending action id. Responses carry `display_text`, whether confirmation is needed, the pending action id and the full draft.
- Status updates to the phone: _not decided_.
- Device to phone audio format: _not decided_. Must arrive as 16 kHz mono for Parakeet.
- Phone to device BLE messages: _not decided_.
- E-ink text budget: _waiting on panel choice_.

**Pending action lifecycle**
1. Command text arrives. If an action is already pending, reject the new command (one at a time).
2. Nemotron runs. Read-only tools execute immediately. A write tool produces a pending action instead of executing.
3. The device shows the confirm screen. The pending action expires 60 s after the last interaction.
4. Confirm (single press): execute exactly once, show the result, clear the pending action.
5. Fix (hold): the correction text, the original request and the current draft go back to Nemotron, which revises the same draft. Timer resets. Back to step 3.
6. Decline (double press) or expiry: discard, nothing sent anywhere.

**Latency budget** (provisional: starting guesses, replace with measurements)

| Step | Owner | Target (s) | Measured (s) |
| --- | --- | --- | --- |
| Audio from device to phone over BLE | Victor | 0.5 | |
| Parakeet transcription on phone | Carlos | 0.5 | |
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

#### Phase B0: Setup and model verification (PLANNED)
- **Goal:** prove Nemotron 3 Nano tool calling works on Token Factory from this codebase, with secrets handled correctly.
- **Steps:** Python project (FastAPI, `openai` SDK, settings from environment); `.env.example`, `.env` gitignored; look up and record the exact model ID and base URL in CLAUDE.md; one plain completion and one tool-calling call with a toy tool; `/health`; per-hop timestamp logging.
- **Gate:** tool-calling script returns a well-formed tool call with valid JSON arguments in 5 of 5 runs; model ID and base URL recorded; no secrets tracked; Zero Data Retention confirmed on by Orlando.
- **Results (if DONE):**

#### Phase B1: API contract and pending action state machine (PLANNED)
- **Goal:** the request/response contract and the pending action lifecycle, built and tested before real tools exist.
- **Steps:** write the agreed phone to cloud JSON into section 2 with Carlos's sign-off (stop if missing); Postgres schema and migrations for users, pending actions and history; endpoints for command, confirm, fix, decline, cancel; 60 s expiry from last interaction; one action at a time; idempotent confirm; agree the status update transport with Carlos.
- **Gate:** state machine tests pass for expiry, one at a time, confirm after expiry rejected, double confirm executes once, fix resets the timer, decline leaves nothing behind.
- **Results (if DONE):**

#### Phase B2: Agent loop with mock tools (PLANNED)
- **Goal:** Nemotron reliably picks the right tool with the right arguments, in English, against mock data.
- **Steps:** tool schemas for all ten tools; realistic mock calendar, inbox and contacts; system prompt with date, time, timezone, language, text budget and relevant memory; iteration cap on the loop; `display_text` length enforced in code; all-in-one email with recipient resolution and auto subject; unknown or ambiguous recipient asks back; fix revises the existing draft; English eval set of spoken-style commands (relative dates like "tomorrow at 3", ambiguous recipients, search then act, requests that must not trigger a write).
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

#### Phase B6: Demo site support (PLANNED)
- **Goal:** judges can try HaloAI without hardware or a Google login.
- **Steps:** sample data mode as default; endpoints for push to talk or typed input, a mock e-ink screen with the same text budget, live memory and action panels.
- **Gate:** a person with no context completes a calendar flow and an email flow on the demo site.
- **Results (if DONE):**

#### Phase B7: Spanish and bilingual (PLANNED, only after English works end to end)
- **Goal:** the same flows in Spanish, then in mixed English and Spanish.
- **Steps:** add Spanish and mixed commands to the eval set; replies and `display_text` in the user's language; Spanish date phrases ("mañana a las 3"); review the phone's Spanish transcription quality.
- **Gate:** Spanish results reported per category next to English. **Provisional target: Spanish within a few points of English, no English regression.**
- **Results (if DONE):**

### 3.2 Phone (Carlos, `phone/`, branch `Phone-App`)

#### Phase P0: Parakeet on the phone (PLANNED)
- **Goal:** prove English speech to text runs fully on Carlos's phone, fast enough.
- **Steps:** Flutter project for Android; Parakeet v3 on device; transcribe at least 5 English test WAV files (16 kHz mono) of real commands such as "send an email to Victor saying the meeting moved to Thursday" and "block 3 to 4 tomorrow for the gym".
- **Gate:** transcription runs with no network; per-clip text, errors and time recorded here.
- **Results (if DONE):**

#### Phase P1: Bluetooth link and audio from the device (PLANNED)
- **Goal:** receive audio from the device over BLE and transcribe it.
- **Steps:** agree the audio format and BLE messages with Victor; pairing and reconnect handling; decode audio to 16 kHz mono; feed Parakeet.
- **Gate:** a spoken command on the real device comes out as correct text on the phone; BLE transfer time measured against the budget.
- **Results (if DONE):**

#### Phase P2: Backend client (PLANNED)
- **Goal:** the phone talks to the backend with the agreed contract.
- **Steps:** send text with date, time, IANA timezone, language and orientation; receive `display_text` and drafts; forward confirm, fix, decline and cancel; receive status updates with the agreed transport; handle cellular retries without double confirms.
- **Gate:** against the backend in mock mode, a calendar flow and an email flow (including fix and decline) complete from the phone.
- **Results (if DONE):**

#### Phase P3: Works locked in a pocket (PLANNED)
- **Goal:** the app keeps working with the screen locked.
- **Steps:** Android foreground service for Bluetooth, transcription and network; battery optimization exemption if needed.
- **Gate:** with the phone locked for at least 10 minutes, a command from the device still completes end to end.
- **Results (if DONE):**

#### Phase P4: App features (PLANNED)
- **Goal:** settings and memory control.
- **Steps:** orientation setting; wallpaper picker that resizes and dithers to the panel's resolution and color depth, then sends it; Google account link; view, edit and delete memory; full text of long drafts.
- **Gate:** each feature works against the real device or backend; a wallpaper set on the phone shows on the device after it turns off.
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
- **Goal:** the button table in CLAUDE.md, implemented and tested.
- **Steps:** propose Arduino core vs ESP-IDF with tradeoffs and get a decision; hardware-independent gesture detector and state machine (testable on a computer); thin ESP32 layer that reads the button and logs every gesture and transition over serial. No BLE, no screen yet. Timing constants in one config file.
- **Gate:** tests cover every gesture in every state and match the table exactly, including the 1 s confirm guard, auto-off only in Ready, and the 60 s expiry; serial log on real hardware shows correct single, double and hold detection.
- **Results (if DONE):**

#### Phase F1: Hardware choice and e-ink screen (PLANNED)
- **Goal:** a panel that fits the card, driving real screens.
- **Steps:** choose ESP32 variant, microphone, e-ink panel and battery that fit 85.6 x 54 mm; render each state's screen; partial refresh for the status bar, full refresh for final screens; wallpaper stored in flash and shown when off; report resolution and the text budget per orientation to the team.
- **Gate:** every state's screen renders on the real panel; full and partial refresh times measured; text budget written into section 2.
- **Results (if DONE):**

#### Phase F2: Microphone and BLE audio (PLANNED)
- **Goal:** recorded speech reaches the phone in the agreed format.
- **Steps:** capture 16 kHz mono while the button is held; agreed compression or record then send; BLE transfer to the phone.
- **Gate:** Carlos's phone transcribes audio recorded on the device correctly; transfer time measured against the budget.
- **Results (if DONE):**

#### Phase F3: Full device flow (PLANNED)
- **Goal:** the device runs the real flow with the phone.
- **Steps:** screens and gestures wired to BLE messages; confirm guard, 60 s expiry display, auto-off.
- **Gate:** calendar and email flows (confirm, fix, decline, cancel, expiry) work from the device with the phone and backend.
- **Results (if DONE):**

#### Phase F4: Power and battery (PLANNED)
- **Goal:** usable battery life and fast wake.
- **Steps:** compare light sleep (BLE link kept) with deep sleep: wake time, reconnect time, battery cost; choose battery size and a battery life target; safe charging and protection circuit.
- **Gate:** measured wake plus reconnect time and battery life written here; charging circuit reviewed for LiPo safety.
- **Results (if DONE):**

### 3.4 Integration milestones (all three)

- **I1: Text round trip without the device.** Phone sends typed or transcribed text to the backend and shows the reply. Needs B1, P2.
- **I2: First end to end command.** Hold the button, say a calendar command, see the confirm screen, confirm, event appears in the calendar (mock or real). Needs B2, P2, F3.
- **I3: Email flow with fix and decline** on real hardware. Needs B4, P3, F3.
- **I4: Latency measured end to end** on real hardware against the 5 s budget, with per-hop timings filled into section 2.
- **I5: Submission.** See section 5.

Target dates for I1 to I4: _to be set by the team_.

## 4. Open Risks

- Contracts not agreed (phone to cloud fields, status transport, audio format, BLE messages, text budget). They block B1, P1, P2, F1 and F2. Agree them in one team call.
- Prompt injection through tool results (a web page or email telling the model to act). Mitigation: tool output treated as data; the confirmation gate stays the backstop for every write.
- Spoken email addresses transcribe badly. Mitigation: contacts first; dictated addresses always shown in full on the confirm screen.
- Android killing background work. Mitigation: foreground service, tested in P3.
- E-ink refresh time eating the budget. Mitigation: partial refresh for status, one full refresh per final screen.
- Accidental presses in a pocket. Mitigation: 1 s confirm guard; send is never a default.
- Google OAuth testing mode tokens expire every 7 days. Re-authorize before the video and before judging.
- Oracle Always Free ARM capacity is sometimes unavailable when creating instances. Create the server early.
- Token Factory credits are finite. Log token usage per request.
- Spanish and mixed-language quality unknown. Deferred to B7 and P5; demo mixed language only if testing supports it.

## 5. Submission Checklist (due October 30, 2026, 10:00 AM PDT, 12:00 PM in Edinburg, TX)

- [ ] Public repo with MIT license (done) and a README with setup steps for backend, phone app and firmware
- [ ] Demo site live in sample data mode
- [ ] YouTube video under 3 minutes showing the real device, the demo site and one search then act moment (a bilingual moment only if Spanish is ready)
- [ ] Written description of features, naming Nemotron, Parakeet, Nebius Token Factory and Tavily
- [ ] Track selected: Personal AI
- [ ] Google demo account re-authorized before judging, December 1 to 15
