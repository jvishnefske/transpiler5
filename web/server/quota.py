"""The limited free trial meter.

One row per Google account. A live (cache-miss) compile costs one unit; a
cache hit costs nothing and never reaches this module. Two independent
ceilings, because they fail differently:

  * a rolling DAILY allowance, which refills -- the normal "you've used your
    quota today, come back tomorrow" case;
  * an optional LIFETIME cap, which does not -- the backstop against someone
    farming a free tier forever.

SQLite with a single writer is ample here: the whole service is one process
behind Traefik, and the write rate is bounded by MAX_CONCURRENT_COMPILES.
"""

from __future__ import annotations

import sqlite3
import threading
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

from . import config

_SCHEMA = """
CREATE TABLE IF NOT EXISTS trial (
    subject      TEXT PRIMARY KEY,
    email        TEXT,
    used_today   INTEGER NOT NULL DEFAULT 0,
    used_total   INTEGER NOT NULL DEFAULT 0,
    window_start TEXT    NOT NULL,
    first_seen   TEXT    NOT NULL,
    last_seen    TEXT    NOT NULL
);
"""


@dataclass(frozen=True)
class Allowance:
    allowed: bool
    remaining_today: int
    daily_limit: int
    used_total: int
    lifetime_limit: int | None
    reason: str | None = None

    def to_json(self) -> dict[str, object]:
        return {
            "allowed": self.allowed,
            "remaining_today": self.remaining_today,
            "daily_limit": self.daily_limit,
            "used_total": self.used_total,
            "lifetime_limit": self.lifetime_limit,
            "reason": self.reason,
        }


class TrialStore:
    def __init__(self, path: Path | None = None) -> None:
        self.path = Path(path or config.DB_PATH)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(str(self.path), check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._conn.executescript(_SCHEMA)
        self._conn.commit()

    # -- internals --------------------------------------------------------

    def _row(self, subject: str, email: str | None) -> sqlite3.Row:
        now = datetime.now(timezone.utc).isoformat()
        cur = self._conn.execute(
            "SELECT * FROM trial WHERE subject = ?", (subject,)
        )
        row = cur.fetchone()
        if row is None:
            self._conn.execute(
                "INSERT INTO trial (subject, email, window_start, first_seen,"
                " last_seen) VALUES (?, ?, ?, ?, ?)",
                (subject, email, now, now, now),
            )
            self._conn.commit()
            row = self._conn.execute(
                "SELECT * FROM trial WHERE subject = ?", (subject,)
            ).fetchone()
        return row

    @staticmethod
    def _window_expired(window_start: str) -> bool:
        try:
            start = datetime.fromisoformat(window_start)
        except ValueError:
            return True
        if start.tzinfo is None:
            start = start.replace(tzinfo=timezone.utc)
        return datetime.now(timezone.utc) - start >= timedelta(days=1)

    def _refresh_window(self, row: sqlite3.Row) -> sqlite3.Row:
        if not self._window_expired(row["window_start"]):
            return row
        now = datetime.now(timezone.utc).isoformat()
        self._conn.execute(
            "UPDATE trial SET used_today = 0, window_start = ? WHERE subject = ?",
            (now, row["subject"]),
        )
        self._conn.commit()
        return self._conn.execute(
            "SELECT * FROM trial WHERE subject = ?", (row["subject"],)
        ).fetchone()

    # -- public API -------------------------------------------------------

    def check(self, subject: str, email: str | None = None) -> Allowance:
        """Report the allowance without spending it."""
        with self._lock:
            row = self._refresh_window(self._row(subject, email))
            return self._allowance(row)

    def spend(self, subject: str, email: str | None = None) -> Allowance:
        """Consume one unit if available. The returned allowance is post-spend.

        Check and decrement happen under one lock so two concurrent requests
        cannot both pass on the last remaining unit.
        """
        with self._lock:
            row = self._refresh_window(self._row(subject, email))
            allowance = self._allowance(row)
            if not allowance.allowed:
                return allowance
            now = datetime.now(timezone.utc).isoformat()
            self._conn.execute(
                "UPDATE trial SET used_today = used_today + 1,"
                " used_total = used_total + 1, last_seen = ?,"
                " email = COALESCE(?, email) WHERE subject = ?",
                (now, email, subject),
            )
            self._conn.commit()
            row = self._conn.execute(
                "SELECT * FROM trial WHERE subject = ?", (subject,)
            ).fetchone()
            return self._allowance(row)

    def refund(self, subject: str) -> None:
        """Return a unit spent on a compile that failed for OUR reasons.

        A user should not lose trial budget because the service timed out or
        crashed. A compile that fails because their C is unsupported is a real
        result and is NOT refunded -- they got the answer they asked for.
        """
        with self._lock:
            self._conn.execute(
                "UPDATE trial SET used_today = MAX(0, used_today - 1),"
                " used_total = MAX(0, used_total - 1) WHERE subject = ?",
                (subject,),
            )
            self._conn.commit()

    def _allowance(self, row: sqlite3.Row) -> Allowance:
        daily = config.TRIAL_DAILY_COMPILES
        lifetime = config.TRIAL_LIFETIME_COMPILES
        remaining = max(0, daily - int(row["used_today"]))
        used_total = int(row["used_total"])

        if lifetime is not None and used_total >= lifetime:
            return Allowance(
                allowed=False,
                remaining_today=remaining,
                daily_limit=daily,
                used_total=used_total,
                lifetime_limit=lifetime,
                reason=(
                    "This trial account has reached its lifetime limit of "
                    f"{lifetime} live compiles."
                ),
            )
        if remaining <= 0:
            return Allowance(
                allowed=False,
                remaining_today=remaining,
                daily_limit=daily,
                used_total=used_total,
                lifetime_limit=lifetime,
                reason=(
                    f"You've used all {daily} live compiles for today. Cached "
                    "results are still free, and your allowance refills in "
                    "under 24 hours."
                ),
            )
        return Allowance(
            allowed=True,
            remaining_today=remaining,
            daily_limit=daily,
            used_total=used_total,
            lifetime_limit=lifetime,
        )
