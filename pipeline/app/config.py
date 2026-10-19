"""Settings loaded from the environment (and pipeline/.env in development).

Secrets have no defaults. Non-secret values have defaults here and can be
overridden by an environment variable of the same name in upper case.
"""

from functools import lru_cache
from pathlib import Path

from pydantic import SecretStr
from pydantic_settings import BaseSettings, SettingsConfigDict

PIPELINE_DIR = Path(__file__).resolve().parent.parent


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_file=PIPELINE_DIR / ".env",
        env_file_encoding="utf-8",
        env_ignore_empty=True,
        extra="ignore",
    )

    # Secrets (required, no defaults). Database URLs contain the password.
    nebius_api_key: SecretStr
    backend_api_key: SecretStr
    database_url: SecretStr
    test_database_url: SecretStr | None = None

    # Nebius Token Factory. Base URL from docs.tokenfactory.nebius.com/api-reference.
    nebius_base_url: str = "https://api.tokenfactory.nebius.com/v1/"
    # Verified against GET /v1/models, see docs/CLAUDE.md (Environment, Backend). Chosen 2026-10-14 by the
    # B2 eval (Nano 70%, Lightning 84%, Super 85%, Ultra 93.5%; Ultra also the fastest at p90).
    nemotron_model_id: str = "nvidia/Nemotron-3-Ultra-550b-a55b"

    # Locked decision: reasoning off by default, on only for multi-step requests.
    reasoning_enabled: bool = False  # on for every turn (testing only)
    # Search then act is the multi-step case: after a web search the model reads partial schedule
    # tables against today's date, and with reasoning off it took the wrong rows (live, 2026-10-17).
    reasoning_after_search: bool = True
    max_reasoning_tokens: int = 2000  # per reasoning turn: the thinking counts against max_tokens
    # With reasoning on, a poisoned result made the model search again until the turn cap (eval trap_06,
    # 2026-10-19). Live runs never needed more than 2.
    max_searches: int = 2
    # The model calls web_search even when it is no longer offered (it remembers it from the conversation),
    # which costs a turn; a request that searched gets one more turn (eval trap_06 traces, 2026-10-19).
    extra_turns_after_search: int = 1
    reasoning_temperature: float = 0.6  # PROVISIONAL sampling for reasoning turns; greedy can loop
    reasoning_top_p: float = 0.95
    llm_timeout_s: float = 20.0
    # Which processor turns text into results: 'fake' (B1 keyword script) or 'nemotron' (B2 loop).
    # Stays 'fake' on the server until the B2 eval passes.
    processor: str = "fake"
    max_model_turns: int = 4  # loop cap: after this many model calls the request is a failure
    max_output_tokens: int = 400  # per model turn

    # E-ink text budget (characters per line, lines per screen), BRIEFING section 2.
    # Decided 2026-10-13 (Victor, F1): Spleen 16 x 32 on the 800 x 480 badge, margins,
    # status bar and hint line already subtracted. Computed, not yet checked for
    # legibility on the badge; the fallback font (20 x 40) would be 38 x 9. Never hardcode elsewhere.
    screen_landscape_chars: int = 48
    screen_landscape_lines: int = 12
    screen_portrait_chars: int = 28
    screen_portrait_lines: int = 22
    max_answer_chars: int = 200  # plain text answers: a glance, not a page

    # Request lifecycle (BRIEFING section 2). PROVISIONAL values are starting
    # guesses until measured on the real hardware and network.
    expiry_s: int = 60  # decision states expire 60 s after the last interaction
    approve_wait_s: float = 5.0  # approve waits this long, then returns executing
    execution_deadline_s: float = 30.0  # PROVISIONAL: after this an execution is unknown
    processing_deadline_s: float = 20.0  # PROVISIONAL: after this processing is a failure
    slow_threshold_s: float = 3.0  # PROVISIONAL: sends the slow event (PROCESSING SLOW)
    sse_ping_s: float = 10.0  # keepalive comment interval on streams
    call_retention_h: int = 24  # how long call_id results are kept for retries
    # Off until B4: 'real' scoped keys fall back to mock tools. 'mock' keys always do.
    # On: 'real' keys read the connected Google account (app/google_world.py).
    real_tools_enabled: bool = False

    # B4: Google and Tavily. Secrets only in the server's secrets file (set-secret), never in git.
    google_client_id: SecretStr | None = None
    google_client_secret: SecretStr | None = None
    google_redirect_uri: str = "https://api.haloai-cov.io/v1/google/callback"
    token_encryption_key: SecretStr | None = None  # Fernet key for stored Google refresh tokens
    tavily_api_key: SecretStr | None = None
    web_search_timeout_s: float = 6.0  # PROVISIONAL: past this the model answers without the search
    # Tavily: 'basic' (1 credit) or 'advanced' (2 credits). Live, 5 runs each (2026-10-19): advanced found the
    # right next game 5/5 against 3/5, same speed; its answers were terser.
    web_search_depth: str = "advanced"
    # The team's addresses (comma separated, server secret). Real mode sends to any address the user
    # said or has as a contact; sends to addresses outside this list count against a daily cap.
    allowed_recipients: SecretStr | None = None
    outside_recipients_per_day: int = 20  # if the phone key leaks, this bounds what it can send
    oauth_state_minutes: int = 10
    # The part of the real calendar read for each request: conflicts are only checked inside it.
    calendar_past_days: int = 7
    calendar_ahead_days: int = 60
    contacts_cache_s: int = 600  # contacts change rarely; the calendar is read fresh every request


@lru_cache
def get_settings() -> Settings:
    return Settings()
