"""screen_lines: pre-wrapped text that must fit the e-ink budget from config."""

import textwrap

from app.config import Settings


def budget(settings: Settings, orientation: str) -> tuple[int, int]:
    if orientation == "portrait":
        return settings.screen_portrait_chars, settings.screen_portrait_lines
    return settings.screen_landscape_chars, settings.screen_landscape_lines


def wrap(lines: list[str], settings: Settings, orientation: str) -> list[str]:
    """Wrap each line to the character budget and cut to the line budget.

    Long words (an email address) are broken rather than allowed to overflow."""
    chars, max_lines = budget(settings, orientation)
    out: list[str] = []
    for line in lines:
        out.extend(textwrap.wrap(line, width=chars, break_long_words=True) or [""])
    return out[:max_lines]


def fits(lines: list[str], settings: Settings, orientation: str) -> bool:
    chars, max_lines = budget(settings, orientation)
    return len(lines) <= max_lines and all(len(line) <= chars for line in lines)


# B1 placeholder screens for outcome states. B2 and B4 replace them with real content.
OUTCOME_LINES = {
    "processing": ["Working on it"],
    "executing": ["Sending"],
    "success": ["Done"],
    "failure": ["Something went wrong"],
    "execution_uncertain": ["Not sure it went through", "Press to check again"],
    "cancelled": ["Cancelled"],
}
