"""Routing, /api/health, /api/me, /api/examples, and startup robustness.

INTENT: pins the parts of main.py that are easy to break by accident and
impossible to notice without running it.

  * The static frontend is mounted at "/" AFTER the API routes. Starlette
    matches routes in definition order, so the mount must not shadow /api/*.
    If someone moves the mount up, every API call starts returning HTML with
    a 404 and the playground silently dies. This file pins the order.
  * /api/health is served unauthenticated to the internet. It deliberately
    exposes google_client_id -- an OAuth *web* client id, public by design,
    and serving it is what lets the frontend stay a static file with no build
    step. The test pins the response's key set EXACTLY, so adding a field is
    a decision someone has to make on purpose rather than by accident.
  * Startup warms the example cache by running the real compiler. A missing
    or broken compiler must degrade the service to "cached results only", not
    prevent it from booting -- the first thing a bad deploy would do
    otherwise is fail to start, taking the landing page down with it.

Run:  nix develop -c python3 -m pytest web/tests -q
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from web.server import cache as cache_mod
from web.server import compiler, config, examples, main

from conftest import asyncio_test, bearer

# --- routing --------------------------------------------------------------


@asyncio_test
async def test_static_mount_does_not_shadow_the_api(svc):
    """StaticFiles at "/" is last, so /api/* still reaches the API."""
    async with svc.client() as c:
        health = await c.get("/api/health")
        assert health.status_code == 200
        assert health.headers["content-type"].startswith("application/json")

        examples_r = await c.get("/api/examples")
        assert examples_r.status_code == 200
        assert examples_r.headers["content-type"].startswith("application/json")

        # An unknown /api path is a 404, NOT the index page. If the mount ever
        # shadows the API this is the assertion that catches it, because
        # StaticFiles(html=False) would still 404 -- but with HTML.
        missing = await c.get("/api/definitely-not-a-route")
        assert missing.status_code == 404
        assert "<html" not in missing.text.lower()


@asyncio_test
async def test_the_pages_and_their_assets_are_served(svc):
    async with svc.client() as c:
        for path, needle in (("/", "emitrust"), ("/playground", "emitrust")):
            r = await c.get(path)
            assert r.status_code == 200, path
            assert r.headers["content-type"].startswith("text/html"), path
            assert needle in r.text.lower(), path

        for asset, ctype in (("/style.css", "text/css"), ("/app.js", "javascript")):
            r = await c.get(asset)
            assert r.status_code == 200, asset
            assert ctype in r.headers["content-type"], asset


# --- health ---------------------------------------------------------------

# The complete, intentional public surface of /api/health. This endpoint is
# reachable unauthenticated from the internet, so the key set is pinned:
# widening it must be a deliberate edit here, not a side effect elsewhere.
HEALTH_KEYS = {
    "ok",
    "compiler",
    "compiler_present",
    "auth_enabled",
    "google_client_id",
    "cache",
    "daily_limit",
}


@asyncio_test
async def test_health_exposes_exactly_the_public_fields(svc):
    async with svc.client() as c:
        r = await c.get("/api/health")
    assert r.status_code == 200
    body = r.json()
    assert set(body) == HEALTH_KEYS
    assert body["ok"] is True
    assert body["auth_enabled"] is True
    # Public by design: an OAuth *web* client id, not a secret.
    assert body["google_client_id"] == "1234567890-test.apps.googleusercontent.com"
    assert body["daily_limit"] == config.TRIAL_DAILY_COMPILES
    assert set(body["cache"]) == {"hits", "misses"}


@asyncio_test
async def test_health_leaks_no_secret(svc, monkeypatch):
    """Nothing that looks like a credential appears anywhere in the response.

    The service never holds an OAuth client SECRET (it verifies ID tokens and
    never exchanges an auth code), so the strongest available check is that no
    secret-shaped value from the process environment can reach this endpoint.
    """
    monkeypatch.setenv("GOOGLE_CLIENT_SECRET", "GOCSPX-do-not-leak-me")
    monkeypatch.setenv("EMITRUST_SECRET_PROBE", "s3cr3t-do-not-leak-me")
    async with svc.client() as c:
        r = await c.get("/api/health")
    blob = json.dumps(r.json())
    for needle in ("GOCSPX", "do-not-leak-me", "secret", "password", "Bearer"):
        assert needle.lower() not in blob.lower(), needle


@asyncio_test
async def test_health_reports_a_missing_compiler_honestly(svc, monkeypatch):
    monkeypatch.setattr(config, "EMITRUST_CC", Path("/nonexistent/emitrust-cc"))
    async with svc.client() as c:
        r = await c.get("/api/health")
    assert r.json()["compiler_present"] is False


@asyncio_test
async def test_health_reports_the_live_lane_as_disabled_without_a_client_id(
    svc, monkeypatch
):
    monkeypatch.setattr(config, "GOOGLE_CLIENT_ID", "")
    async with svc.client() as c:
        r = await c.get("/api/health")
    body = r.json()
    assert body["auth_enabled"] is False
    assert body["google_client_id"] is None


# --- /api/me --------------------------------------------------------------


@asyncio_test
async def test_me_is_anonymous_without_a_credential(svc):
    async with svc.client() as c:
        r = await c.get("/api/me")
    assert r.status_code == 200
    assert r.json() == {
        "signed_in": False,
        "auth_enabled": True,
        "daily_limit": config.TRIAL_DAILY_COMPILES,
    }
    # Asking who you are must not cost a trial unit.
    assert svc.trial.spend_calls == []


@asyncio_test
async def test_me_with_an_invalid_credential_is_anonymous_not_an_error(svc):
    """/api/me is advisory; a bad token means "not signed in", not a 401."""
    svc.credential_is_invalid()
    async with svc.client() as c:
        r = await c.get("/api/me", headers=bearer())
    assert r.status_code == 200
    assert r.json()["signed_in"] is False


@asyncio_test
async def test_me_reports_the_allowance_without_spending_it(svc):
    svc.signed_in_as("sub-mallory", email="mallory@example.com")
    async with svc.client() as c:
        r = await c.get("/api/me", headers=bearer())
    body = r.json()
    assert body["signed_in"] is True
    assert body["user"] == {
        "email": "mallory@example.com",
        "name": "Test User",
        "picture": None,
    }
    assert body["trial"]["allowed"] is True
    assert body["trial"]["used_total"] == 0
    assert svc.trial.spend_calls == []
    assert svc.trial.used_total("sub-mallory") == 0


# --- /api/examples --------------------------------------------------------


@asyncio_test
async def test_examples_report_which_entries_are_free_right_now(svc):
    """`cached` on the menu is what the frontend uses to promise "free"."""
    async with svc.client() as c:
        r = await c.get("/api/examples")
    entries = r.json()["examples"]
    assert len(entries) == len(examples.CATALOG)
    assert all(e["cached"] is False for e in entries)  # nothing warmed here

    # Warm exactly one, and only that one flips.
    first = examples.CATALOG[0]
    key = compiler.cache_key(
        first.source(), compiler.CompileOptions.parse(first.options)
    )
    svc.cache.put(key, {"ok": True, "output": "", "diagnostics": ""})
    async with svc.client() as c:
        r = await c.get("/api/examples")
    entries = r.json()["examples"]
    assert entries[0]["slug"] == first.slug
    assert entries[0]["cached"] is True
    assert all(e["cached"] is False for e in entries[1:])


# --- startup --------------------------------------------------------------


@asyncio_test
async def test_startup_survives_a_missing_compiler(svc, monkeypatch):
    """A bad deploy must not take the landing page down with it."""
    monkeypatch.setattr(config, "EMITRUST_CC", Path("/nonexistent/emitrust-cc"))
    svc.compile_must_not_run()
    async with main.lifespan(main.app):
        async with svc.client() as c:
            assert (await c.get("/api/health")).status_code == 200
            assert (await c.get("/")).status_code == 200


@asyncio_test
async def test_startup_survives_a_compiler_that_raises(svc, monkeypatch):
    monkeypatch.setattr(config, "EMITRUST_CC", Path(__file__))  # exists
    svc.compile_raises(RuntimeError("the compiler exploded"))
    async with main.lifespan(main.app):
        async with svc.client() as c:
            assert (await c.get("/api/health")).status_code == 200
    warmed, failed = await examples.warm_cache(svc.cache)
    assert (warmed, failed) == (0, len(examples.CATALOG))


@asyncio_test
async def test_startup_survives_an_unwritable_cache(svc, monkeypatch):
    """The state dir may be misowned on a fresh box. Boot anyway."""
    monkeypatch.setattr(config, "EMITRUST_CC", Path(__file__))

    def _explode(key, value):
        raise OSError(13, "Permission denied")

    monkeypatch.setattr(svc.cache, "put", _explode)
    svc.compile_returns(
        compiler.CompileResult(
            ok=True, output="", diagnostics="", exit_code=0, duration_ms=1,
            emit="rust",
        )
    )
    async with main.lifespan(main.app):
        async with svc.client() as c:
            assert (await c.get("/api/health")).status_code == 200


@asyncio_test
async def test_startup_does_not_cache_a_timed_out_example(svc, monkeypatch):
    monkeypatch.setattr(config, "EMITRUST_CC", Path(__file__))
    svc.compile_returns(
        compiler.CompileResult(
            ok=False, output="", diagnostics="timed out", exit_code=124,
            duration_ms=1, emit="rust",
        )
    )
    warmed, failed = await examples.warm_cache(svc.cache)
    assert warmed == 0
    assert failed == len(examples.CATALOG)
    assert svc.cache.stats()["hits"] == 0


@asyncio_test
async def test_a_warmed_example_is_free_to_an_anonymous_caller(svc, monkeypatch):
    """The whole point of the startup warm, checked through the HTTP layer."""
    monkeypatch.setattr(config, "EMITRUST_CC", Path(__file__))
    svc.compile_returns(
        compiler.CompileResult(
            ok=True, output="fn main() {}\n", diagnostics="", exit_code=0,
            duration_ms=1, emit="rust",
        )
    )
    warmed, failed = await examples.warm_cache(svc.cache)
    assert (warmed, failed) == (len(examples.CATALOG), 0)

    svc.compile_must_not_run()
    ex = examples.CATALOG[0]
    async with svc.client() as c:
        r = await c.post(
            "/api/compile", json={"source": ex.source(), "options": ex.options}
        )
    assert r.status_code == 200, r.text
    assert r.json()["cached"] is True
    assert svc.trial.spend_calls == []
