"""The emitrust web service.

Serves the static frontend and a small JSON API. The one interesting rule is
the free/paid boundary, and it is enforced in exactly one place -- `/api/compile`:

    cache HIT   -> served to anyone, no account, no meter.
    cache MISS  -> requires a verified Google identity AND trial budget.

That ordering is deliberate. The cache lookup happens BEFORE the auth check,
so an anonymous visitor can exercise every feature on the playground against
the prebaked examples and only meets the sign-in wall when they ask for
something genuinely new. It also means the meter measures compiler work, not
page views.
"""

from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import auth, cache, compiler, config, examples, quota

log = logging.getLogger("emitrust.web")

_cache = cache.ResultCache()
_trial = quota.TrialStore()


@asynccontextmanager
async def lifespan(app: FastAPI):
    warmed, failed = await examples.warm_cache(_cache)
    log.info("example cache warm: %d ready, %d failed", warmed, failed)
    if failed:
        # Not fatal: the affected example simply falls into the paid lane.
        log.warning(
            "%d example(s) are not cached and will require sign-in", failed
        )
    yield


app = FastAPI(
    title="emitrust",
    description="C and C++ to readable Rust, as a service.",
    lifespan=lifespan,
)

if config.CORS_ORIGINS:
    app.add_middleware(
        CORSMiddleware,
        allow_origins=config.CORS_ORIGINS,
        allow_methods=["GET", "POST"],
        allow_headers=["Content-Type", "Authorization"],
    )


# --- request models ------------------------------------------------------


class CompileRequest(BaseModel):
    source: str = Field(..., description="C or C++ translation unit")
    options: dict[str, Any] | None = None


# --- helpers -------------------------------------------------------------


def _bearer(authorization: str | None) -> str | None:
    if not authorization:
        return None
    scheme, _, token = authorization.partition(" ")
    if scheme.lower() != "bearer" or not token:
        return None
    return token.strip()


def _identity(authorization: str | None) -> auth.Identity | None:
    """Best-effort identity. Returns None when absent or invalid."""
    token = _bearer(authorization)
    if not token:
        return None
    try:
        return auth.verify(token)
    except (auth.AuthError, auth.AuthUnavailable):
        return None


# --- API -----------------------------------------------------------------


@app.get("/api/health")
async def health() -> dict[str, Any]:
    return {
        "ok": True,
        "compiler": str(config.EMITRUST_CC),
        "compiler_present": config.EMITRUST_CC.exists(),
        "auth_enabled": auth.enabled(),
        # Public by design: an OAuth *web* client id is not a secret, and
        # serving it here lets the frontend stay a static file with no build
        # step. The client SECRET is never used by this service at all -- we
        # only verify ID tokens, we never exchange an auth code.
        "google_client_id": config.GOOGLE_CLIENT_ID or None,
        "cache": _cache.stats(),
        "daily_limit": config.TRIAL_DAILY_COMPILES,
    }


@app.get("/api/examples")
async def list_examples() -> dict[str, Any]:
    """The free lane's menu. Every entry here is prewarmed into the cache."""
    return {"examples": examples.catalog(_cache)}


@app.get("/api/me")
async def me(authorization: str | None = Header(default=None)) -> dict[str, Any]:
    identity = _identity(authorization)
    if identity is None:
        return {
            "signed_in": False,
            "auth_enabled": auth.enabled(),
            "daily_limit": config.TRIAL_DAILY_COMPILES,
        }
    allowance = _trial.check(identity.subject, identity.email)
    return {
        "signed_in": True,
        "auth_enabled": True,
        "user": identity.to_json(),
        "trial": allowance.to_json(),
    }


@app.post("/api/compile")
async def compile_endpoint(
    body: CompileRequest,
    authorization: str | None = Header(default=None),
) -> JSONResponse:
    try:
        options = compiler.CompileOptions.parse(body.options)
    except compiler.OptionError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    if not body.source.strip():
        raise HTTPException(status_code=400, detail="source is empty")

    key = compiler.cache_key(body.source, options)

    # --- free lane: an exact result we already have -----------------------
    hit = _cache.get(key)
    if hit is not None:
        return JSONResponse({**hit, "cached": True, "key": key})

    # --- paid lane: this is new work, so it needs an account --------------
    token = _bearer(authorization)
    try:
        identity = auth.verify(token)
    except auth.AuthUnavailable as exc:
        return JSONResponse(
            status_code=503,
            content={
                "error": "live_compile_unavailable",
                "detail": str(exc),
                "cached": False,
            },
        )
    except auth.AuthError as exc:
        return JSONResponse(
            status_code=401,
            content={
                "error": "sign_in_required",
                "detail": (
                    "This exact source and option set has not been compiled "
                    "before, so it needs a live run. Sign in with Google to "
                    "start your limited free trial."
                ),
                "reason": str(exc) if token else None,
                "cached": False,
                "daily_limit": config.TRIAL_DAILY_COMPILES,
            },
        )

    allowance = _trial.spend(identity.subject, identity.email)
    if not allowance.allowed:
        return JSONResponse(
            status_code=429,
            content={
                "error": "trial_exhausted",
                "detail": allowance.reason,
                "trial": allowance.to_json(),
                "cached": False,
            },
        )

    try:
        result = await compiler.compile_source(body.source, options)
    except compiler.OptionError as exc:
        _trial.refund(identity.subject)
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception:
        # Our fault, not theirs -- give the trial unit back.
        _trial.refund(identity.subject)
        log.exception("compile failed")
        raise HTTPException(
            status_code=500, detail="the compiler service failed"
        ) from None

    payload = result.to_json()
    # A timeout is not a durable answer -- caching it would poison the free
    # lane with a failure that a retry might not reproduce.
    if result.exit_code != 124:
        _cache.put(key, payload)

    return JSONResponse(
        {
            **payload,
            "cached": False,
            "key": key,
            "trial": allowance.to_json(),
        }
    )


# --- static frontend -----------------------------------------------------
#
# Mounted last so it cannot shadow /api/*.

_frontend = Path(config.FRONTEND_DIR)
if _frontend.is_dir():

    @app.get("/")
    async def index() -> FileResponse:
        return FileResponse(_frontend / "index.html")

    @app.get("/playground")
    async def playground() -> FileResponse:
        return FileResponse(_frontend / "playground.html")

    app.mount("/", StaticFiles(directory=str(_frontend)), name="static")
