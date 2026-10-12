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
    # Verified against GET /v1/models, see docs/CLAUDE.md (Environment, Backend).
    nemotron_model_id: str = "nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B"

    # Locked decision: reasoning off by default, on only for multi-step requests.
    reasoning_enabled: bool = False
    llm_timeout_s: float = 20.0

    # E-ink text budget (characters per line, lines per screen).
    # PROVISIONAL: placeholder assuming a 24 x 40 px fixed-width cell on the
    # 800 x 480 panel. Victor replaces these in F1. Never hardcode elsewhere.
    screen_landscape_chars: int = 33
    screen_landscape_lines: int = 12
    screen_portrait_chars: int = 20
    screen_portrait_lines: int = 20

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
    real_tools_enabled: bool = False


@lru_cache
def get_settings() -> Settings:
    return Settings()
