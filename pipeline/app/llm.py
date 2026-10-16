"""Nemotron client on Nebius Token Factory (OpenAI-compatible endpoint)."""

from functools import lru_cache

from openai import AsyncOpenAI, OpenAI

from app.config import get_settings


@lru_cache
def get_client() -> OpenAI:
    s = get_settings()
    return OpenAI(
        base_url=s.nebius_base_url,
        api_key=s.nebius_api_key.get_secret_value(),
        timeout=s.llm_timeout_s,
    )


@lru_cache
def get_async_client() -> AsyncOpenAI:
    """For the server: waiting on the model never blocks other requests."""
    s = get_settings()
    return AsyncOpenAI(
        base_url=s.nebius_base_url,
        api_key=s.nebius_api_key.get_secret_value(),
        timeout=s.llm_timeout_s,
    )


def model_kwargs(reasoning: bool | None = None) -> dict:
    """Model ID plus the reasoning switch.

    Nemotron 3 Nano toggles reasoning through its chat template
    (enable_thinking). The OpenAI SDK has no field for it, so it goes in
    extra_body and the server passes it to the template.
    """
    s = get_settings()
    enabled = s.reasoning_enabled if reasoning is None else reasoning
    kwargs: dict = {
        "model": s.nemotron_model_id,
        "extra_body": {"chat_template_kwargs": {"enable_thinking": enabled}},
    }
    if not enabled:
        # Model card: greedy decoding when reasoning is off.
        kwargs["temperature"] = 0.0
    return kwargs
