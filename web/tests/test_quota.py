"""TrialStore.spend/check/refund at the store level.

INTENT: pin the distinction that spend() and check() answer DIFFERENT
questions, because conflating them was a real, shipped bug.

`spend()` answers "did THIS request get its unit?". `check()` answers "would
the NEXT one?". The original code computed spend()'s answer by re-evaluating
check()'s predicate after the decrement, so the last unit of an allowance came
back as `allowed=False` -- with the unit already deducted. The endpoint then
returned 429 for a compile the user had just been charged for, every single
day, and a daily limit of N behaved like N-1 with the Nth unit burned.

The API-level companion to this is test_compile_boundary.py's exhaustion
tests; this file pins it where the arithmetic lives, in isolation from HTTP.

Run:  nix develop -c python3 -m pytest web/tests -q
"""

from __future__ import annotations

import pytest

from web.server import config, quota


@pytest.fixture
def store(tmp_path, monkeypatch):
    monkeypatch.setattr(config, "TRIAL_DAILY_COMPILES", 3)
    monkeypatch.setattr(config, "TRIAL_LIFETIME_COMPILES", None)
    return quota.TrialStore(tmp_path / "trial.sqlite3")


def test_every_unit_of_the_allowance_is_usable(store):
    """A daily limit of 3 must grant three compiles, not two."""
    granted = [store.spend("u").allowed for _ in range(3)]
    assert granted == [True, True, True]
    assert store.spend("u").allowed is False


def test_the_last_unit_is_granted_not_charged_and_refused(store):
    """The exact shape of the bug: allowed=False with the unit already gone."""
    store.spend("u")
    store.spend("u")
    last = store.spend("u")
    assert last.allowed is True
    assert last.reason is None
    # ...and the counters are post-spend, so the UI can say "0 left".
    assert last.remaining_today == 0
    assert last.used_total == 3
    # The NEXT one is the refusal, and it costs nothing.
    denied = store.spend("u")
    assert denied.allowed is False
    assert denied.used_total == 3
    assert "used all 3 live compiles" in denied.reason


def test_the_last_lifetime_unit_is_usable_too(store, monkeypatch):
    monkeypatch.setattr(config, "TRIAL_LIFETIME_COMPILES", 2)
    assert store.spend("u").allowed is True
    last = store.spend("u")
    assert last.allowed is True
    assert last.used_total == 2
    denied = store.spend("u")
    assert denied.allowed is False
    assert "lifetime limit of 2" in denied.reason


def test_check_never_spends(store):
    assert store.check("u").remaining_today == 3
    assert store.check("u").remaining_today == 3
    assert store.check("u").used_total == 0


def test_refund_restores_exactly_one_unit(store):
    store.spend("u")
    store.spend("u")
    store.refund("u")
    assert store.check("u").remaining_today == 2
    assert store.check("u").used_total == 1


def test_refund_cannot_manufacture_credit(store):
    """A refund on an account that never spent must not go negative."""
    store.refund("never-seen")
    store.refund("never-seen")
    assert store.check("never-seen").used_total == 0
    assert store.check("never-seen").remaining_today == 3


def test_accounts_are_independent(store):
    store.spend("u1")
    store.spend("u1")
    assert store.check("u1").remaining_today == 1
    assert store.check("u2").remaining_today == 3


def test_the_daily_window_refills(store, monkeypatch):
    for _ in range(3):
        store.spend("u")
    assert store.spend("u").allowed is False
    # Age the window past 24h.
    store._conn.execute(
        "UPDATE trial SET window_start = '2000-01-01T00:00:00+00:00'"
        " WHERE subject = 'u'"
    )
    store._conn.commit()
    refreshed = store.check("u")
    assert refreshed.allowed is True
    assert refreshed.remaining_today == 3
    # The lifetime counter does NOT refill -- that is the point of having it.
    assert refreshed.used_total == 3
