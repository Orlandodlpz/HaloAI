# BRIEFING.md: HaloAI AI Pipeline Execution Plan

**Last Updated:** 2026-10-06
**Purpose:** The operational plan: what we're building, how the pieces fit, and the phased sequence to get there with minimal rework.

> Relationship to other docs: CLAUDE.md tracks decisions and state. This document is the **execution plan**.

## 1. What We're Building

A cloud backend that receives a transcribed voice command from the phone app, uses Nemotron 3 Nano (Nebius Token Factory) in a tool-calling loop to turn it into a calendar or email action, holds that action as a pending draft until the user confirms on the device, and then executes it against Google Calendar or Gmail. It also stores persistent memory (contacts, preferences, recent actions) and serves the demo site's sample data mode.

**Inputs:** command text from the phone, plus the phone's current date, time, IANA timezone, detected language (English for now; Spanish and mixed later) and screen orientation; confirm, fix and decline signals from the device via the phone; fix text when the user holds to correct a draft.
**Outputs:** short `display_text` that fits the e-ink screen, the full draft for the phone app, status updates for meaningful steps, and executed actions in Google Calendar and Gmail.
**Constraint driving every decision:** about 5 seconds from button release to the final screen for a simple command, without ever executing an irreversible action that the user did not confirm.

## 2. Integration Map

```
ESP32 device  --BLE audio-->  Android phone (Parakeet v3)  --HTTPS text-->  Backend (this repo)
ESP32 device  <--BLE screen--  Android phone               <--HTTPS reply--  Backend

Backend --> Nebius Token Factory (Nemotron 3 Nano, Zero Data Retention on)
Backend --> Google Calendar, Gmail send, Google People "other contacts"
Backend --> Tavily web search
Backend --> Postgres (memory, pending actions, history)
Demo site --> Backend (sample data mode uses mock tools)
```

Who produces and consumes what:
- **Phone app (Carlos)** produces command text and button signals; consumes `display_text`, status updates and full drafts.
- **Device (Victor)** owns gestures, the 1 s confirm guard, auto-off and the e-ink rendering. The backend never sees raw gestures, only the resulting signal (confirm, fix with text, decline, cancel).
- **Backend (Orlando)** owns the agent loop, pending action state, tool execution, memory and the demo site API.

**Interface contract (phone to cloud): fields NOT YET WRITTEN DOWN.** Transport is agreed (HTTPS over Wi-Fi or cellular). Before building endpoints, write the agreed request and response JSON here, signed off by Orlando and Carlos. It must cover at least: a session or user identifier, command text, current time with timezone, language, orientation, and the signals confirm / fix (with text) / decline / cancel keyed by a pending action id. The response must carry `display_text`, whether confirmation is needed, the pending action id, and the full draft.

**Pending action lifecycle:**
1. Command text arrives. If an action is already pending, reject the new command (one at a time).
2. Agent loop runs. Read-only tools execute immediately. A write tool produces a pending action instead of executing.
3. Response shows the confirm screen. The pending action expires 60 s after the last interaction.
4. Confirm: execute exactly once (idempotent), return the result, clear the pending action.
5. Fix (hold): the correction text plus the original request plus the current draft go back to Nemotron, which revises the same draft. Timer resets. Back to step 3.
6. Decline (double press) or expiry: discard, nothing sent anywhere.

## 3. Phases

Each phase ends with a gate. The gate must pass before the next phase begins. Completed phases get marked **DONE** with results and gotchas recorded.

### Phase 0: Setup and model verification (PLANNED)
- **Goal:** prove Nemotron 3 Nano tool calling works on Token Factory from this codebase, with secrets handled correctly.
- **Steps:** create the Python project (FastAPI, `openai` SDK, settings from environment); add `.env.example` and gitignore `.env`; look up and record the exact model ID and base URL; make one plain completion call and one tool-calling call with a toy tool; confirm Zero Data Retention is on for the account; add a `/health` endpoint; set up per-hop timestamp logging.
- **Gate:** a script makes a tool-calling request and the model returns a well-formed tool call with valid JSON arguments, at least 5 runs out of 5; the model ID and base URL are recorded in CLAUDE.md; `git status` shows no secrets tracked; ZDR status confirmed by Orlando.
- **Results (if DONE):**

### Phase 1: API contract and pending action state machine (PLANNED)
- **Goal:** the request/response contract and the pending action lifecycle, built and tested before any real tools exist.
- **Steps:** write the agreed JSON contract into section 2 (stop and ask if it is still missing); Postgres schema and migrations for users, pending actions and history; endpoints for command, confirm, fix, decline, cancel; 60 s expiry from last interaction; one action at a time; idempotent confirm; decide with Carlos how status updates reach the phone.
- **Gate:** state machine tests pass for expiry, one at a time, confirm after expiry rejected, double confirm executes once, fix resets the timer, decline leaves nothing behind.
- **Results (if DONE):**

### Phase 2: Agent loop with mock tools (PLANNED)
- **Goal:** Nemotron reliably picks the right tool with the right arguments, in English, against mock data.
- **Steps:** tool schemas for all ten tools; mock implementations with realistic sample calendar, inbox and contacts; system prompt with date, time, timezone, language, text budget and relevant memory; loop with a maximum number of iterations; `display_text` length enforced in code, not only in the prompt; all-in-one email with recipient resolution and auto subject; ambiguous or unknown recipient asks back; fix flow revises the existing draft; build an English eval set of spoken-style commands (relative dates like "tomorrow at 3", ambiguous recipients, search then act).
- **Gate:** eval set run with tool choice and argument correctness reported per category. **Provisional target: at least 90% correct tool and arguments overall, and 100% on "no write action without a pending confirmation".** The 90% is not validated; replace it after the first eval run shows what is realistic. Median latency of the model loop reported.
- **Results (if DONE):**

### Phase 3: Memory (PLANNED)
- **Goal:** persistent contacts, preferences and recent actions that change what the assistant does.
- **Steps:** Postgres tables for contacts (names, nicknames, addresses), preferences and history; `remember` and `recall` tools; inject only relevant memory into the prompt; CRUD endpoints so the app and demo site can view, edit and delete memory.
- **Gate:** "email Victor" resolves from memory without dictation; an edited or deleted memory changes the next answer; "move that meeting to 4" finds the right event from history.
- **Results (if DONE):**

### Phase 4: Real integrations (PLANNED)
- **Goal:** the same agent loop working against real Google services and Tavily through the dedicated demo account.
- **Steps:** OAuth (testing mode, demo account as test user) with token storage outside git; real Calendar, Gmail send and People "other contacts" implementations behind the same interface; Tavily search; a setting to switch mock and real per session.
- **Gate:** end to end on the demo account: create an event, send an email to a team member, and one search then act request. Verified from logs that Gmail was not called before confirmation, and that a declined or expired draft left nothing in Gmail.
- **Results (if DONE):**

### Phase 5: Deployment and latency (PLANNED)
- **Goal:** always-on backend on the Oracle ARM server, inside the latency budget.
- **Steps:** deploy (service manager or containers), HTTPS with a real certificate, Postgres on the server with daily backups, environment secrets on the server, timing dashboard or log summary per hop; tune reasoning and prompt size for speed.
- **Gate:** backend share of the budget measured on the deployed server over at least 20 requests. **Provisional targets: Nemotron loop about 1.2 s and one Google or Tavily call about 0.6 s, for a simple command.** These are starting guesses from the team's budget, not measurements; replace them with measured values. Re-run the Phase 2 eval set on the deployed config to confirm no accuracy regression from speed tuning.
- **Results (if DONE):**

### Phase 6: Demo site support (PLANNED)
- **Goal:** judges can try HaloAI without the hardware or a Google login.
- **Steps:** sample data mode as the default; endpoints the demo site needs (push to talk or typed input, mock e-ink screen with the same text budget, live memory and action panels).
- **Gate:** a person with no context completes a calendar flow and an email flow on the demo site in sample data mode.
- **Results (if DONE):**

### Phase 7: Spanish and bilingual (PLANNED, starts only after English works end to end)
- **Goal:** the same flows working in Spanish, then in mixed English and Spanish sentences.
- **Steps:** add Spanish and mixed-language commands to the eval set; check replies and `display_text` come back in the user's language; check date phrases in Spanish ("mañana a las 3"); review Carlos's speech to text results for Spanish and mixed sentences, since transcription errors upstream cap what the pipeline can do.
- **Gate:** Spanish eval results reported per category alongside English. **Provisional target: Spanish within a few points of English accuracy, and no English regression.** The team decides from the mixed-language results whether to demo mixed sentences or one language at a time.
- **Results (if DONE):**

## 4. Open Risks

- Phone to cloud JSON fields not written down. Mitigation: Phase 1 starts by writing them here with Carlos's sign-off.
- Status update transport (streaming vs polling) undecided. Decide with Carlos in Phase 1; it affects the endpoint design.
- E-ink text budget unknown until the panel is chosen. Keep it as configuration.
- Prompt injection through tool results (a web page or email telling the model to send something). Mitigation: treat tool output as data in the prompt, and the confirmation gate stays the backstop for every write.
- Spoken email addresses transcribe badly. Mitigation: contacts first; dictated addresses always shown in full on the confirm screen.
- Spanish and mixed English and Spanish are deferred to Phase 7. Mixed sentences may confuse speech to text upstream, so demo mixed language only if Phase 7 testing supports it.
- Google OAuth testing mode tokens expire every 7 days. Re-authorize before the video and before judging.
- Oracle Always Free capacity is sometimes unavailable when creating ARM instances. Create the server early.
- Token Factory credits are finite. Log token usage per request.
