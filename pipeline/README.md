# HaloAI backend (`pipeline/`)

The cloud half of HaloAI: it turns a transcribed command into one proposed action with **NVIDIA Nemotron 3 Ultra on Nebius Token Factory**, holds it until the user presses the badge, then executes it exactly once in Google Calendar or Gmail. It also serves the demo site's API. Python 3.11 or newer, FastAPI, Postgres.

Full design, contracts and measured results: [`docs/BRIEFING.md`](../docs/BRIEFING.md) (section 2 for the API, section 3.1 for the backend phases). Conventions and gotchas: [`docs/CLAUDE.md`](../docs/CLAUDE.md).

## Quick start (local, sample data)

Needs Python 3.11+ and Docker. About 5 minutes.

```bash
cd pipeline
python3 -m venv .venv && .venv/bin/pip install -e '.[dev]'
cp .env.example .env            # then fill in the Required block (see below)
docker compose up -d            # Postgres 16 on 127.0.0.1:5433, databases haloai and haloai_test
.venv/bin/python scripts/migrate.py && .venv/bin/python scripts/migrate.py --test
.venv/bin/python -m pytest -q   # about 280 tests, no network, no model calls
```

Run the backend with the real model on sample data (no Google account needed):

```bash
PROCESSOR=nemotron .venv/bin/uvicorn app.main:app --workers 1   # exactly one worker, see below
```

Try it in a browser with the demo site (set `DEMO_SITE_KEY` in `.env` to any random string first):

```bash
.venv/bin/python scripts/dev_site.py   # http://127.0.0.1:8080
```

Or exercise every API flow from the command line, the way the phone app does:

```bash
python3 scripts/phone_sim.py all                          # against the live server, sample data
python3 scripts/phone_sim.py create --base http://127.0.0.1:8000 --key YOUR_KEY --real   # a local backend
```

### The `.env` file

The **Required** block of [`.env.example`](.env.example): `NEBIUS_API_KEY` (a Token Factory key; turn on Zero Data Retention for the account), `BACKEND_API_KEY` (any long random string; it becomes the phone's key), and the Postgres password and URLs (`postgresql://haloai:<password>@127.0.0.1:5433/haloai` and `.../haloai_test`). Everything else has a default in [`app/config.py`](app/config.py). Never commit `.env`.

Check the model ID against your key: `.venv/bin/python scripts/list_models.py`.

## How a command flows

1. The phone sends text (never audio) to `POST /v1/requests` with the phone's date, time zone and screen orientation, and gets Server-Sent Events back: `accepted`, `status` lines for the e-ink status bar, an optional `slow`, then one `result`.
2. [`app/agent.py`](app/agent.py) runs a plain tool calling loop against Token Factory's OpenAI compatible API (the `openai` SDK with Nebius's base URL; no agent framework). The model sees a short system prompt ([`app/prompt.py`](app/prompt.py)) and 10 tools ([`app/tools.py`](app/tools.py)). Read tools return data marked as untrusted; proposal tools end the loop with REVIEW, a schedule conflict, a "which one?" choice, or a question.
3. **The model proposes, code decides.** Contact resolution, conflict detection, the IANA time zone, recipient rules and the screen text are code. The model cannot send email; only the executor can, after approval.
4. The result is stored as a revision with a proposal hash and a deterministic **execution key**. `POST /v1/requests/{id}/approve` binds to that exact revision and hash, so a retry, a second press or a reboot never executes twice ([`app/lifecycle.py`](app/lifecycle.py)). A proposal expires after 60 seconds.
5. The executor ([`app/google_executor.py`](app/google_executor.py), or the sample world in [`app/mockstore.py`](app/mockstore.py)) carries it out and the result goes back to the badge.

## Modes

| Who calls | World | Executor |
| --- | --- | --- |
| Demo site and its visitors (scope `mock`) | Each visitor's own sample week; real Tavily search, capped per day | Sample calendar and outbox: nothing real is sent |
| Phone key (scope `real`) with `REAL_TOOLS_ENABLED=false` | The phone user's sample world | Sample |
| Phone key with `REAL_TOOLS_ENABLED=true` | The connected Google account (Calendar, contacts, "other contacts") and Tavily | **Google Calendar and Gmail, for real** |

One rule (`uses_real_tools` in [`app/processing.py`](app/processing.py)) picks both the world and the executor, and every draft records which world it came from, so a sample proposal can never reach the real executor or the reverse.

**Connecting Google** (real mode): create an OAuth client (Web application) in Google Cloud with the Calendar events, Gmail send and People (contacts and other contacts) scopes, set `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET`, `GOOGLE_REDIRECT_URI` and a `TOKEN_ENCRYPTION_KEY` (`python3 -c "from cryptography.fernet import Fernet; print(Fernet.generate_key().decode())"`), then `POST /v1/google/connect` with the phone key returns a sign in link. In Google's testing mode the sign in expires after 7 days.

## Safety rules, enforced in code

- Nothing changes without a press: only `approve` on the displayed revision executes, exactly once.
- Tool results (web pages, event text, invitations) are data, never instructions. An email address the user never said, and that is not a contact, cannot become a recipient. Memory learns only from approved actions and from the user's own "remember that..." or "save a routine...".
- Every API call needs a key; demo keys only reach sample data; the demo's model spend and web searches are capped per day.
- Timeouts and unknown outcomes are reported as uncertain, never as sent.

## The eval

100 spoken style commands in [`evals/b2_en.jsonl`](evals/b2_en.jsonl), across calendar, email, ambiguous names, conflicts, missing information, additions, memory, routines and prompt injection traps.

```bash
.venv/bin/python scripts/eval_b2.py --check           # validate the cases against the tool code, no model
.venv/bin/python scripts/eval_b2.py --repeat 3        # the full eval, 3 runs (about 7 minutes, about $1)
.venv/bin/python scripts/eval_b2.py --only trap --repeat 3
```

Token Factory is not deterministic even at temperature 0, so every change is judged on 3 runs. Results are saved in [`evals/results/`](evals/results/). Latest: 290 of 300 (96.7%), traps 27 of 27. Do not run it while people are using the demo: it shares the account's capacity.

## Deployment

The live server (`https://api.haloai-cov.io` and the demo at `https://haloai-cov.io`) is an Oracle Cloud ARM machine with Caddy in front, a systemd unit and native Postgres. [`scripts/deploy.sh`](scripts/deploy.sh) deploys `origin/main` only: it checks the new code starts before swapping it in, runs migrations, installs the units and the [`Caddyfile`](deploy/Caddyfile), publishes the demo site and checks `/health` (which answers `401` without a key). Nightly encrypted backups go to Object Storage ([`deploy/backup.sh`](deploy/backup.sh)); [`scripts/restore_check.sh`](scripts/restore_check.sh) proves a restore.

**Run uvicorn with exactly one worker.** Retries, stream reattach and rate limits live in process memory.

## Layout

```
app/          FastAPI app: lifecycle, streams, tools, agent loop, Google, Tavily, memory, demo endpoints
migrations/   numbered SQL migrations (scripts/migrate.py)
site/         the demo site: a badge simulator drawn with the firmware's own font bytes
evals/        the eval cases and saved results
scripts/      deploy, migrate, keys, eval, phone simulator, local demo site, restore check
deploy/       systemd units, Caddyfile, backup script
tests/        pytest, against a real Postgres and fake Google, Tavily and model servers
```
