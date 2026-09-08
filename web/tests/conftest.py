"""Shared fixtures for the web API tests.

INTENT: make it possible to exercise `web/server/main.py` -- the FastAPI app
that had never been executed -- in-process, hermetically, and with the
free/paid boundary observable.

THE ORDERING TRAP, and why this file exists at all: `web/server/config.py`
reads os.environ at IMPORT time. Any `from web.server import ...` that happens
before the environment is set silently binds the DEPLOYMENT defaults, and
`main.py` builds its cache and trial store at import time too -- so an
unlucky import order would have the tests writing into /var/lib/emitrust-web.
pytest imports conftest.py before it imports any test module, so setting the
environment here, at module scope, is the only ordering that is guaranteed.

Everything else is per-test: each test gets a fresh ResultCache and a fresh
TrialStore under its own tmp_path, patched over the module-level singletons in
main.py, so no test can see another's cached results or spent trial units.

The compiler is REFUSED by default (see Svc.compile_must_not_run). "No compile
ran" is half of what the 401/400/429 paths are actually promising, so the
default has to be a stub that fails loudly rather than a mock that quietly
returns something.
"""

from __future__ import annotations

import asyncio
import functools
import os
import sys
import tempfile
from contextlib import asynccontextmanager
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

# --- environment, before the first web.server import ----------------------

_SESSION_STATE = Path(tempfile.mkdtemp(prefix="emitrust-web-apitests-"))
os.environ["EMITRUST_STATE_DIR"] = str(_SESSION_STATE)
os.environ["EMITRUST_CACHE_DIR"] = str(_SESSION_STATE / "cache")
os.environ["EMITRUST_DB"] = str(_SESSION_STATE / "trial.sqlite3")
# Forced, not defaulted: an ambient GOOGLE_CLIENT_ID on the developer's box
# would flip the 503-vs-401 tests. Tests that need the live lane set
# config.GOOGLE_CLIENT_ID explicitly via the `svc` fixture.
os.environ["GOOGLE_CLIENT_ID"] = ""

import httpx  # noqa: E402

from web.server import auth, cache, compiler, config, main, quota  # noqa: E402

# A syntactically plausible OAuth web client id. Never used against Google:
# every test that needs a verified identity patches auth.verify.
TEST_CLIENT_ID = "1234567890-test.apps.googleusercontent.com"


def asyncio_test(fn):
    """Run an async test on its own event loop.

    Deliberately not pytest-asyncio/anyio: the suite must run with nothing but
    the packages the service itself needs, and one loop per test also keeps a
    stray loop-bound object from leaking between tests.
    """

    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        asyncio.run(fn(*args, **kwargs))

    return wrapper


class CountingTrialStore(quota.TrialStore):
    """A real trial store that also records that it was touched.

    A cache hit must not *call* the meter at all -- not "call it and spend
    zero" -- so the tests need call counts, not just balances.
    """

    def __init__(self, path: Path) -> None:
        super().__init__(path)
        self.spend_calls: list[str] = []
        self.refund_calls: list[str] = []

    def spend(self, subject, email=None):
        self.spend_calls.append(subject)
        return super().spend(subject, email)

    def refund(self, subject):
        self.refund_calls.append(subject)
        return super().refund(subject)

    def used_total(self, subject: str) -> int:
        row = self._conn.execute(
            "SELECT used_total FROM trial WHERE subject = ?", (subject,)
        ).fetchone()
        return 0 if row is None else int(row["used_total"])


class Svc:
    """The app under test, plus handles on everything it is allowed to touch."""

    def __init__(self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
        self.monkeypatch = monkeypatch
        self.cache = cache.ResultCache(tmp_path / "cache")
        self.trial = CountingTrialStore(tmp_path / "trial.sqlite3")
        monkeypatch.setattr(main, "_cache", self.cache)
        monkeypatch.setattr(main, "_trial", self.trial)
        monkeypatch.setattr(config, "GOOGLE_CLIENT_ID", TEST_CLIENT_ID)
        # compiler._semaphore is a module-global asyncio.Semaphore; one loop
        # per test means a semaphore created under a previous loop must not
        # survive into this one.
        monkeypatch.setattr(compiler, "_semaphore", None)
        self.compile_calls: list[tuple[str, compiler.CompileOptions]] = []
        self.compile_must_not_run()

    # -- compiler control -------------------------------------------------

    def compile_must_not_run(self) -> None:
        """The default. Any compile from here is a failed test, loudly."""

        async def _forbidden(source, options):
            raise AssertionError(
                "the compiler ran for a request that must never reach it"
            )

        self.monkeypatch.setattr(compiler, "compile_source", _forbidden)

    def compile_returns(self, result: compiler.CompileResult) -> None:
        async def _stub(source, options):
            self.compile_calls.append((source, options))
            return result

        self.monkeypatch.setattr(compiler, "compile_source", _stub)

    def compile_raises(self, exc: BaseException) -> None:
        async def _stub(source, options):
            self.compile_calls.append((source, options))
            raise exc

        self.monkeypatch.setattr(compiler, "compile_source", _stub)

    def compile_for_real(self, require_binary: bool = True) -> None:
        """Restore the genuine compile_source. Skips if the tool is missing.

        require_binary=False is for the checks that live INSIDE compile_source
        (the source-size limit) and never reach the executable.
        """
        cc = Path(
            os.environ.get("EMITRUST_CC", REPO / "build" / "tools" / "emitrust-cc")
        )
        if require_binary and not cc.exists():
            pytest.skip(f"emitrust-cc not built at {cc}")
        self.monkeypatch.setattr(config, "EMITRUST_CC", cc)
        self.monkeypatch.setattr(compiler, "compile_source", _REAL_COMPILE_SOURCE)

    # -- identity control -------------------------------------------------

    def signed_in_as(self, subject: str, email: str | None = None) -> None:
        """Patch auth.verify, NOT the endpoint. Token minting is Google's job.

        One test (test_auth.py) still drives the real verify(), so the wiring
        between main.py and google-auth is not entirely mocked away.
        """
        identity = auth.Identity(
            subject=subject, email=email or f"{subject}@example.com",
            name="Test User", picture=None,
        )

        def _verify(token):
            if not token:
                raise auth.AuthError("no credential supplied")
            return identity

        self.monkeypatch.setattr(auth, "verify", _verify)

    def credential_is_invalid(self, reason: str = "Token expired") -> None:
        def _verify(token):
            if not token:
                raise auth.AuthError("no credential supplied")
            raise auth.AuthError(reason)

        self.monkeypatch.setattr(auth, "verify", _verify)

    # -- http -------------------------------------------------------------

    @asynccontextmanager
    async def client(self):
        transport = httpx.ASGITransport(app=main.app)
        async with httpx.AsyncClient(
            transport=transport, base_url="http://testserver"
        ) as c:
            yield c


# Captured before any test can patch it over.
_REAL_COMPILE_SOURCE = compiler.compile_source


@pytest.fixture
def svc(tmp_path, monkeypatch):
    return Svc(tmp_path, monkeypatch)


def bearer(token: str = "an.opaque.credential") -> dict[str, str]:
    return {"Authorization": f"Bearer {token}"}
