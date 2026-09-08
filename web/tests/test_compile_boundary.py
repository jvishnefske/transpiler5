"""POST /api/compile: the free/paid boundary, end to end through FastAPI.

INTENT: this file pins the ONE security-relevant rule in the service --

    cache HIT   -> served to anyone. No account, no meter, no compiler.
    cache MISS  -> requires a verified Google identity AND trial budget.

and the ordering that makes it work: the cache lookup happens BEFORE the auth
check. Everything here is a boundary test, so each case pins BOTH halves of
the promise -- the status code the caller sees, and what the server did NOT do
(no compile, no trial unit spent, no unit lost to our own failure).

The central design claim, and the reason this file exists, is
test_live_compile_makes_the_result_free_for_everyone_afterwards: a paid
compile must deposit its result in the free lane, so the very next ANONYMOUS
request for the same source is a free hit. That is what makes the trial meter
measure compiler work rather than page views, and it had never been executed.

Rejection wordings are asserted verbatim. They are the product's voice on the
one screen where a user is told they must pay, and a silent reword is a
behavior change.

Run:  nix develop -c python3 -m pytest web/tests -q
"""

from __future__ import annotations

import re

import pytest

from web.server import compiler, config

from conftest import REPO, asyncio_test, bearer

SRC = "int main(void) { return 0; }\n"
OTHER_SRC = "int main(void) { return 1; }\n"

# Verbatim, from main.py's 401 arm. A user who is told to sign in deserves to
# be told exactly why, and this is that sentence.
SIGN_IN_DETAIL = (
    "This exact source and option set has not been compiled "
    "before, so it needs a live run. Sign in with Google to "
    "start your limited free trial."
)
# Verbatim, from auth.AuthUnavailable in auth.verify().
UNAVAILABLE_DETAIL = (
    "This deployment has no Google client id configured, so live "
    "compiles are disabled. Cached results still work."
)


def fake_result(**kw) -> compiler.CompileResult:
    base = dict(
        ok=True,
        output="fn main() {}\n",
        diagnostics="",
        exit_code=0,
        duration_ms=7,
        emit="rust",
    )
    base.update(kw)
    return compiler.CompileResult(**base)


def prewarm(svc, source=SRC, options=None, output="cached fn main() {}\n"):
    """Put a result in the cache the way the startup warm would."""
    opts = compiler.CompileOptions.parse(options)
    key = compiler.cache_key(source, opts)
    svc.cache.put(key, fake_result(output=output).to_json())
    return key


# --- 1. the free lane -----------------------------------------------------


@asyncio_test
async def test_cache_hit_is_free_and_anonymous(svc):
    """A prewarmed result is served with no credential and no metering."""
    key = prewarm(svc)
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC})

    assert r.status_code == 200, r.text
    body = r.json()
    assert body["cached"] is True
    assert body["key"] == key
    assert body["output"] == "cached fn main() {}\n"
    # The meter was not merely charged zero -- it was never consulted.
    assert svc.trial.spend_calls == []
    assert svc.trial.refund_calls == []
    # And the compiler never ran: svc's default stub would have raised.


@asyncio_test
async def test_cache_hit_is_free_even_with_no_client_id(svc, monkeypatch):
    """A misconfigured deployment still serves the whole free lane."""
    monkeypatch.setattr(config, "GOOGLE_CLIENT_ID", "")
    prewarm(svc)
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC})
    assert r.status_code == 200, r.text
    assert r.json()["cached"] is True
    assert svc.trial.spend_calls == []


@asyncio_test
async def test_a_cache_hit_carries_the_whole_result_shape(svc):
    """The free lane must not be a degraded lane.

    app.js reads files/emit/duration_ms/truncated off the compile response and
    does not special-case a hit, so a cached answer has to be the same shape as
    a live one -- plus `cached` and `key`.
    """
    prewarm(svc)
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC})
    assert set(r.json()) == {
        "ok", "output", "diagnostics", "exit_code", "duration_ms", "emit",
        "truncated", "files", "cached", "key",
    }


# --- 2/3. the wall -------------------------------------------------------


@asyncio_test
async def test_cache_miss_without_credential_is_401(svc):
    """New work with no account: 401, no compile, no unit spent."""
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC})

    assert r.status_code == 401, r.text
    body = r.json()
    assert body["error"] == "sign_in_required"
    assert body["detail"] == SIGN_IN_DETAIL
    # No token was offered, so there is no rejection reason to report.
    assert body["reason"] is None
    assert body["cached"] is False
    assert body["daily_limit"] == config.TRIAL_DAILY_COMPILES
    assert svc.trial.spend_calls == []
    assert svc.compile_calls == []


@asyncio_test
async def test_cache_miss_with_invalid_credential_is_401_not_500(svc):
    """A forged/expired token is the user's problem: 401, never 500, never 200."""
    svc.credential_is_invalid("Token expired, iat 1234")
    async with svc.client() as c:
        r = await c.post(
            "/api/compile", json={"source": SRC}, headers=bearer()
        )

    assert r.status_code == 401, r.text
    body = r.json()
    assert body["error"] == "sign_in_required"
    assert body["detail"] == SIGN_IN_DETAIL
    # A credential WAS offered, so the reason why it failed is reported.
    assert body["reason"] == "Token expired, iat 1234"
    assert svc.trial.spend_calls == []
    assert svc.compile_calls == []


@pytest.mark.parametrize(
    "header",
    [
        {"Authorization": "an.opaque.credential"},  # no scheme
        {"Authorization": "Basic dXNlcjpwdw=="},  # wrong scheme
        {"Authorization": "Bearer"},  # scheme, no token
        {"Authorization": "Bearer   "},  # scheme, blank token
    ],
    ids=["no-scheme", "wrong-scheme", "no-token", "blank-token"],
)
@asyncio_test
async def test_malformed_authorization_header_is_401_not_a_crash(svc, header):
    svc.signed_in_as("sub-should-not-be-used")
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC}, headers=header)
    assert r.status_code == 401, r.text
    assert r.json()["error"] == "sign_in_required"
    assert svc.trial.spend_calls == []


# --- 4. the paid lane, and what it deposits in the free one ---------------


@asyncio_test
async def test_live_compile_makes_the_result_free_for_everyone_afterwards(svc):
    """The design's central claim, against the REAL compiler.

    A signed-in user pays one unit for a live compile; the result lands in the
    cache; the very next request for the same source -- ANONYMOUS, no header
    at all -- is served free and without running the compiler again.
    """
    svc.compile_for_real()
    svc.signed_in_as("sub-alice")

    async with svc.client() as c:
        first = await c.post(
            "/api/compile", json={"source": SRC}, headers=bearer()
        )
        assert first.status_code == 200, first.text
        body1 = first.json()
        assert body1["cached"] is False
        assert body1["ok"] is True
        assert body1["exit_code"] == 0
        assert "fn main" in body1["output"]
        assert body1["trial"]["used_total"] == 1
        assert svc.trial.spend_calls == ["sub-alice"]
        assert svc.trial.refund_calls == []
        assert svc.trial.used_total("sub-alice") == 1

        # From here the compiler is forbidden: if the second request compiles
        # anything at all, the free lane is a lie and the test fails loudly.
        svc.compile_must_not_run()

        second = await c.post("/api/compile", json={"source": SRC})

    assert second.status_code == 200, second.text
    body2 = second.json()
    assert body2["cached"] is True
    assert body2["key"] == body1["key"]
    assert body2["output"] == body1["output"]
    assert body2["diagnostics"] == body1["diagnostics"]
    # Exactly one unit, ever, for a result that is now free to the world.
    assert svc.trial.spend_calls == ["sub-alice"]
    assert svc.trial.used_total("sub-alice") == 1


@asyncio_test
async def test_a_real_rejection_is_a_paid_answer_that_then_becomes_free(svc):
    """The REAL compiler refusing real C: still a result, still cached.

    Rejection is a feature in this project, so a located diagnostic is the
    answer the user asked for -- it costs a unit, it is not refunded, and the
    next visitor gets that same refusal for free.
    """
    svc.compile_for_real()
    svc.signed_in_as("sub-karl")
    source = (REPO / "web" / "examples" / "rejected.c").read_text()

    async with svc.client() as c:
        paid = await c.post(
            "/api/compile", json={"source": source}, headers=bearer()
        )
        assert paid.status_code == 200, paid.text
        body = paid.json()
        assert body["ok"] is False
        assert body["exit_code"] != 0
        # A LOCATED diagnostic -- file:line:col -- not a bare failure.
        assert re.search(r"\.c:\d+:\d+: error:", body["diagnostics"]), body[
            "diagnostics"
        ]
        assert svc.trial.refund_calls == []
        assert svc.trial.used_total("sub-karl") == 1

        svc.compile_must_not_run()
        free = await c.post("/api/compile", json={"source": source})

    assert free.status_code == 200, free.text
    assert free.json()["cached"] is True
    assert free.json()["diagnostics"] == body["diagnostics"]
    assert svc.trial.used_total("sub-karl") == 1


@asyncio_test
async def test_live_compile_spends_exactly_one_unit(svc):
    svc.compile_returns(fake_result())
    svc.signed_in_as("sub-bob")
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC}, headers=bearer())
    assert r.status_code == 200, r.text
    assert len(svc.compile_calls) == 1
    assert svc.trial.used_total("sub-bob") == 1
    assert r.json()["trial"]["remaining_today"] == config.TRIAL_DAILY_COMPILES - 1


# --- 5. exhaustion --------------------------------------------------------


@asyncio_test
async def test_trial_exhaustion_is_429_and_cached_results_still_work(svc, monkeypatch):
    monkeypatch.setattr(config, "TRIAL_DAILY_COMPILES", 1)
    svc.compile_returns(fake_result(output="first answer\n"))
    svc.signed_in_as("sub-carol")

    async with svc.client() as c:
        first = await c.post(
            "/api/compile", json={"source": SRC}, headers=bearer()
        )
        assert first.status_code == 200, first.text
        assert first.json()["cached"] is False

        # The allowance is now zero. New work is refused...
        svc.compile_must_not_run()
        second = await c.post(
            "/api/compile", json={"source": OTHER_SRC}, headers=bearer()
        )
        assert second.status_code == 429, second.text
        body = second.json()
        assert body["error"] == "trial_exhausted"
        assert body["detail"] == (
            "You've used all 1 live compiles for today. Cached "
            "results are still free, and your allowance refills in "
            "under 24 hours."
        )
        assert body["cached"] is False
        assert body["trial"]["allowed"] is False
        assert body["trial"]["remaining_today"] == 0

        # ...but the free lane is untouched, for this same exhausted user.
        third = await c.post(
            "/api/compile", json={"source": SRC}, headers=bearer()
        )
        assert third.status_code == 200, third.text
        assert third.json()["cached"] is True
        assert third.json()["output"] == "first answer\n"

    # The refused request cost nothing: still exactly one unit used.
    assert svc.trial.used_total("sub-carol") == 1


@asyncio_test
async def test_lifetime_cap_is_429_too(svc, monkeypatch):
    monkeypatch.setattr(config, "TRIAL_DAILY_COMPILES", 100)
    monkeypatch.setattr(config, "TRIAL_LIFETIME_COMPILES", 1)
    svc.compile_returns(fake_result())
    svc.signed_in_as("sub-dave")
    async with svc.client() as c:
        assert (
            await c.post("/api/compile", json={"source": SRC}, headers=bearer())
        ).status_code == 200
        svc.compile_must_not_run()
        r = await c.post(
            "/api/compile", json={"source": OTHER_SRC}, headers=bearer()
        )
    assert r.status_code == 429, r.text
    assert r.json()["error"] == "trial_exhausted"
    assert r.json()["detail"] == (
        "This trial account has reached its lifetime limit of 1 live compiles."
    )


# --- 6. misconfiguration is not an auth failure ---------------------------


@asyncio_test
async def test_miss_with_no_client_id_is_503_not_401(svc, monkeypatch):
    """Telling a user to sign in when signing in cannot work would be a lie."""
    monkeypatch.setattr(config, "GOOGLE_CLIENT_ID", "")
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC})

    assert r.status_code == 503, r.text
    body = r.json()
    assert body["error"] == "live_compile_unavailable"
    assert body["detail"] == UNAVAILABLE_DETAIL
    assert body["cached"] is False
    assert svc.trial.spend_calls == []
    assert svc.compile_calls == []


@asyncio_test
async def test_miss_with_no_client_id_is_503_even_with_a_credential(svc, monkeypatch):
    monkeypatch.setattr(config, "GOOGLE_CLIENT_ID", "")
    async with svc.client() as c:
        r = await c.post(
            "/api/compile", json={"source": SRC}, headers=bearer()
        )
    assert r.status_code == 503, r.text
    assert r.json()["error"] == "live_compile_unavailable"


# --- 7. option validation -------------------------------------------------


@pytest.mark.parametrize(
    "options,detail",
    [
        ({"bogus": 1}, "unknown option(s): bogus"),
        ({"emit": "exe"}, "emit must be one of actor-plan, crate, mlir, rust"),
        ({"language": "python"}, "language must be 'c' or 'cpp'"),
        ({"actor_mode": "fibers"}, "actor_mode must be one of async, same-thread, threaded"),
        ({"recover": "yes"}, "recover must be a boolean"),
        ({"incremental": True}, "incremental output requires emit=crate"),
        (
            {"actor_mode": "threaded", "no_actor_lift": True},
            "actor_mode requires the actor lift; clear 'no actor lift'",
        ),
    ],
    ids=[
        "unknown-key",
        "bad-emit",
        "bad-language",
        "bad-actor-mode",
        "non-bool",
        "incremental-without-crate",
        "contradictory-pair",
    ],
)
@asyncio_test
async def test_bad_options_are_400_before_anything_else(svc, options, detail):
    """400, not 500 -- and rejected before the auth check and the meter."""
    svc.signed_in_as("sub-erin")
    async with svc.client() as c:
        r = await c.post(
            "/api/compile",
            json={"source": SRC, "options": options},
            headers=bearer(),
        )
    assert r.status_code == 400, r.text
    assert r.json() == {"detail": detail}
    assert svc.trial.spend_calls == []
    assert svc.compile_calls == []


@asyncio_test
async def test_empty_source_is_400(svc):
    svc.signed_in_as("sub-frank")
    async with svc.client() as c:
        r = await c.post(
            "/api/compile", json={"source": "   \n"}, headers=bearer()
        )
    assert r.status_code == 400, r.text
    assert r.json() == {"detail": "source is empty"}
    assert svc.trial.spend_calls == []


@asyncio_test
async def test_missing_source_field_is_422(svc):
    """Pydantic's own validation error -- a 422, still not a 500."""
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"options": {}})
    assert r.status_code == 422, r.text


# --- 9. the refund policy -------------------------------------------------


@asyncio_test
async def test_compiler_crash_refunds_the_unit(svc):
    """Our failure, not theirs: 500, and the unit comes back."""
    svc.compile_raises(RuntimeError("boom"))
    svc.signed_in_as("sub-grace")
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC}, headers=bearer())

    assert r.status_code == 500, r.text
    assert r.json() == {"detail": "the compiler service failed"}
    assert svc.trial.spend_calls == ["sub-grace"]
    assert svc.trial.refund_calls == ["sub-grace"]
    assert svc.trial.used_total("sub-grace") == 0


@asyncio_test
async def test_timeout_refunds_the_unit_and_is_not_cached(svc):
    """A timeout is not a durable answer, so it costs the user nothing.

    It must also stay OUT of the cache: caching it would poison the free lane
    with a failure a retry might not reproduce.
    """
    svc.compile_returns(
        fake_result(ok=False, output="", diagnostics="timed out after 20s.", exit_code=124)
    )
    svc.signed_in_as("sub-heidi")
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC}, headers=bearer())
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["exit_code"] == 124
        assert body["cached"] is False

        # Not cached: a second anonymous request is a miss, i.e. a 401.
        svc.compile_must_not_run()
        again = await c.post("/api/compile", json={"source": SRC})
        assert again.status_code == 401

    assert svc.trial.refund_calls == ["sub-heidi"]
    assert svc.trial.used_total("sub-heidi") == 0
    # The trial block in the response reflects the refund, not the spend.
    assert body["trial"]["used_total"] == 0


@asyncio_test
async def test_a_cache_write_failure_still_returns_the_paid_answer(svc):
    """A full disk must not cost the user both the unit and the result.

    The cache write is a deposit into the FREE lane. If it fails the answer
    still exists and has already been paid for, so it is handed over; only the
    "free for the next visitor" part is lost.
    """

    def _explode(key, value):
        raise OSError(28, "No space left on device")

    svc.monkeypatch.setattr(svc.cache, "put", _explode)
    svc.compile_returns(fake_result(output="paid for this\n"))
    svc.signed_in_as("sub-liam")
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC}, headers=bearer())
    assert r.status_code == 200, r.text
    assert r.json()["output"] == "paid for this\n"
    assert r.json()["cached"] is False


@asyncio_test
async def test_unsupported_c_is_a_real_answer_and_is_not_refunded(svc):
    """A rejection is the answer they asked for. It costs a unit and it caches.

    Rejection is a feature in this project; a located diagnostic is a result,
    not a service failure, so refunding it would be wrong -- and so would
    forcing the next person to pay for the same rejection again.
    """
    svc.compile_returns(
        fake_result(
            ok=False,
            output="",
            diagnostics="input.c:3:5: error: unsupported construct",
            exit_code=1,
        )
    )
    svc.signed_in_as("sub-ivan")
    async with svc.client() as c:
        r = await c.post("/api/compile", json={"source": SRC}, headers=bearer())
        assert r.status_code == 200, r.text
        assert r.json()["ok"] is False
        assert r.json()["exit_code"] == 1
        assert svc.trial.refund_calls == []
        assert svc.trial.used_total("sub-ivan") == 1

        # ...and the rejection is now free for the next visitor.
        svc.compile_must_not_run()
        again = await c.post("/api/compile", json={"source": SRC})
    assert again.status_code == 200, again.text
    assert again.json()["cached"] is True
    assert again.json()["diagnostics"] == "input.c:3:5: error: unsupported construct"


@asyncio_test
async def test_oversized_source_is_400_and_refunded(svc, monkeypatch):
    """The size check lives past the meter, so it must hand the unit back."""
    monkeypatch.setattr(config, "MAX_SOURCE_BYTES", 2048)
    # The real compile_source, because the size check lives inside it -- but
    # the binary is never reached, so this test does not need a built tree.
    svc.compile_for_real(require_binary=False)
    svc.signed_in_as("sub-judy")
    async with svc.client() as c:
        r = await c.post(
            "/api/compile", json={"source": "x" * 4096}, headers=bearer()
        )
    assert r.status_code == 400, r.text
    assert r.json() == {"detail": "source exceeds 2 KiB"}
    assert svc.trial.refund_calls == ["sub-judy"]
    assert svc.trial.used_total("sub-judy") == 0


# --- the cache key really does separate option sets -----------------------


@asyncio_test
async def test_a_different_option_set_is_a_different_request(svc):
    """A hit for emit=rust must not answer a request for emit=mlir."""
    prewarm(svc, options={"emit": "rust"})
    async with svc.client() as c:
        hit = await c.post(
            "/api/compile", json={"source": SRC, "options": {"emit": "rust"}}
        )
        assert hit.status_code == 200
        miss = await c.post(
            "/api/compile", json={"source": SRC, "options": {"emit": "mlir"}}
        )
    assert miss.status_code == 401, miss.text
    assert svc.compile_calls == []
