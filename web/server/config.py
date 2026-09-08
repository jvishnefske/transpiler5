"""Runtime configuration, read once from the environment.

Everything that differs between a laptop and the .202 box lives here, so no
other module reads os.environ. Defaults are the .202 deployment's values --
running with no environment at all should still boot, just with an empty
Google client id (which disables the live-compile lane rather than crashing).
"""

import os
from pathlib import Path

# --- paths ---------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]

# The transpiler driver. On .202 this is the meson-built binary; override
# when running the service from a different checkout.
EMITRUST_CC = Path(
    os.environ.get("EMITRUST_CC", REPO_ROOT / "build" / "tools" / "emitrust-cc")
)

# The nix devshell wrapper. emitrust-cc links the nix LLVM/clang dylibs, so
# outside the devshell it will not start. Empty string = invoke directly
# (correct when the service itself runs inside `nix develop`).
NIX_DEVELOP = os.environ.get("EMITRUST_NIX_DEVELOP", "")

STATE_DIR = Path(os.environ.get("EMITRUST_STATE_DIR", "/var/lib/emitrust-web"))
CACHE_DIR = Path(os.environ.get("EMITRUST_CACHE_DIR", STATE_DIR / "cache"))
DB_PATH = Path(os.environ.get("EMITRUST_DB", STATE_DIR / "trial.sqlite3"))
FRONTEND_DIR = Path(
    os.environ.get("EMITRUST_FRONTEND", REPO_ROOT / "web" / "frontend")
)
EXAMPLES_DIR = Path(
    os.environ.get("EMITRUST_EXAMPLES", REPO_ROOT / "web" / "examples")
)

# --- auth ----------------------------------------------------------------

# Google OAuth 2.0 web client id (the "...apps.googleusercontent.com" one).
# Empty disables the live lane: cached results still serve, uncached requests
# answer 503 with a clear reason instead of 401. That is deliberate -- a
# misconfigured deployment should not look like an auth failure to the user.
GOOGLE_CLIENT_ID = os.environ.get("GOOGLE_CLIENT_ID", "")

# --- limited free trial --------------------------------------------------

# Live (uncached) compiles allowed per signed-in account per rolling day.
TRIAL_DAILY_COMPILES = int(os.environ.get("EMITRUST_TRIAL_DAILY", "25"))

# Hard ceiling on a single trial account, ever. None = no lifetime cap.
_lifetime = os.environ.get("EMITRUST_TRIAL_LIFETIME", "500")
TRIAL_LIFETIME_COMPILES = int(_lifetime) if _lifetime.lower() != "none" else None

# --- sandbox limits ------------------------------------------------------
#
# These bound a SINGLE emitrust-cc invocation. They are defense in depth, not
# the isolation boundary -- see web/README.md, "Threat model". The boundary is
# the container/VM the service runs in.

MAX_SOURCE_BYTES = int(os.environ.get("EMITRUST_MAX_SOURCE", str(256 * 1024)))
COMPILE_TIMEOUT_S = float(os.environ.get("EMITRUST_TIMEOUT", "20"))
COMPILE_CPU_SECONDS = int(os.environ.get("EMITRUST_CPU_SECONDS", "25"))
COMPILE_ADDRESS_SPACE = int(
    os.environ.get("EMITRUST_ADDRESS_SPACE", str(8 * 1024 * 1024 * 1024))
)
COMPILE_MAX_OUTPUT_BYTES = int(
    os.environ.get("EMITRUST_MAX_OUTPUT", str(4 * 1024 * 1024))
)

# Concurrent live compiles across the whole service. Each invocation is itself
# multi-threaded (MLIR grows a verifier thread pool sized to the machine), so
# this number multiplies rather than adds -- keep it well under core count.
MAX_CONCURRENT_COMPILES = int(os.environ.get("EMITRUST_CONCURRENCY", "4"))

# --- http ----------------------------------------------------------------

# Origins allowed to call the API. The frontend is served same-origin in the
# default deployment, so this matters only if you split them.
CORS_ORIGINS = [
    o.strip()
    for o in os.environ.get(
        "EMITRUST_CORS_ORIGINS", "https://vishnefske.com"
    ).split(",")
    if o.strip()
]

# Trust X-Forwarded-* from the reverse proxy. On .202 the only ingress is
# Traefik at 192.168.1.185, so this is safe; do not enable it on a box
# reachable directly from the internet.
#
# INFORMATIONAL ONLY -- nothing reads this. The switch that actually matters
# is uvicorn's `--proxy-headers --forwarded-allow-ips` in
# web/deploy/emitrust-web.service; setting EMITRUST_TRUST_PROXY=0 does not
# turn anything off. Kept (documented, not deleted) so the discrepancy is
# visible rather than a knob that quietly lies.
TRUST_PROXY_HEADERS = os.environ.get("EMITRUST_TRUST_PROXY", "1") == "1"
