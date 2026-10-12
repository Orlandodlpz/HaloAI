"""Time source. Code asks the clock for now() instead of calling datetime directly,
so tests can move time forward (for example past the 60 s expiry) without sleeping."""

from datetime import datetime, timedelta, timezone


class SystemClock:
    def now(self) -> datetime:
        return datetime.now(timezone.utc)


class FakeClock:
    def __init__(self, start: datetime | None = None):
        self._now = start or datetime(2026, 10, 12, 20, 0, tzinfo=timezone.utc)

    def now(self) -> datetime:
        return self._now

    def advance(self, seconds: float) -> None:
        self._now += timedelta(seconds=seconds)
