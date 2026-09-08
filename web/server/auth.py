"""Google Sign-In verification.

The browser runs Google Identity Services and hands us an ID token (a JWT).
We verify it against Google's published keys and trust nothing else -- in
particular we never accept a client-supplied user id, email or "signed in"
boolean, because those are exactly what an attacker would forge to skip the
trial meter.

Verification is delegated to `google.auth`, which checks the signature, the
issuer, the audience (our client id) and expiry. Doing that by hand is how
people ship auth bypasses.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from . import config


class AuthError(Exception):
    """Token missing, malformed, expired, or for the wrong audience."""


class AuthUnavailable(Exception):
    """The deployment has no GOOGLE_CLIENT_ID, so the live lane is disabled.

    Deliberately distinct from AuthError: this is our misconfiguration, not
    the user's, and the API answers 503 rather than 401 so nobody is told to
    "sign in" when signing in cannot possibly work.
    """


@dataclass(frozen=True)
class Identity:
    subject: str  # Google's stable per-account id ("sub"). The trial key.
    email: str | None
    name: str | None
    picture: str | None

    def to_json(self) -> dict[str, str | None]:
        return {"email": self.email, "name": self.name, "picture": self.picture}


def enabled() -> bool:
    return bool(config.GOOGLE_CLIENT_ID)


# A compact JWS is three base64url segments separated by dots (RFC 7515 s7.1).
# Padding is not produced by that serialization but is accepted here, because
# this check exists to throw out obvious garbage -- it is NOT the verifier.
_JWT_SHAPE = re.compile(r"^[A-Za-z0-9_=-]+\.[A-Za-z0-9_=-]+\.[A-Za-z0-9_=-]+$")

# Google ID tokens run well under 2 KiB. The bound is generous; its job is to
# stop an unauthenticated caller from handing megabytes to a JWT parser.
MAX_CREDENTIAL_BYTES = 8192


def _looks_like_a_jwt(token: str) -> bool:
    return len(token) <= MAX_CREDENTIAL_BYTES and bool(_JWT_SHAPE.match(token))


def verify(id_token_str: str | None) -> Identity:
    """Verify a Google ID token. Raises AuthError / AuthUnavailable."""
    if not enabled():
        raise AuthUnavailable(
            "This deployment has no Google client id configured, so live "
            "compiles are disabled. Cached results still work."
        )
    if not id_token_str:
        raise AuthError("no credential supplied")

    # Reject structurally impossible credentials before anything else. Two
    # reasons, both about the fact that this path faces the internet
    # unauthenticated: verification below fetches Google's signing certs over
    # the network, so without this check any caller could make us issue an
    # outbound HTTPS request for a string like "hello"; and token rejection
    # should not depend on Google being reachable.
    if not _looks_like_a_jwt(id_token_str):
        raise AuthError("credential is not a JWT")

    try:
        from google.auth import exceptions as google_exceptions
        from google.auth.transport import requests as google_requests
        from google.oauth2 import id_token as google_id_token
    except ImportError as exc:  # pragma: no cover - deployment error
        # The message carries the real ImportError: the usual cause is the
        # missing `google-auth[requests]` extra, and `import google.auth`
        # succeeding makes that gap otherwise invisible.
        raise AuthUnavailable(
            f"google-auth is not usable on the server: {exc}"
        ) from exc

    try:
        claims = google_id_token.verify_oauth2_token(
            id_token_str,
            google_requests.Request(),
            config.GOOGLE_CLIENT_ID,
        )
    except google_exceptions.TransportError as exc:
        # We could not reach Google. That is our problem, not the user's, so
        # it must surface as 503 -- never as "your sign-in failed".
        raise AuthUnavailable(
            f"could not reach Google to verify the credential: {exc}"
        ) from exc
    except (ValueError, KeyError, google_exceptions.GoogleAuthError) as exc:
        # verify_oauth2_token raises ValueError for a bad signature/audience
        # /expiry, GoogleAuthError for a wrong issuer, and can KeyError on a
        # token with no `iss` claim. All three mean the same thing to us: this
        # credential is not acceptable. Letting any of them escape would turn
        # a stale token into a 500 on the compile endpoint.
        raise AuthError(str(exc) or type(exc).__name__) from exc

    subject = claims.get("sub")
    if not subject:
        raise AuthError("token carries no subject")

    # verify_oauth2_token already checks `aud` against our client id and the
    # issuer, but it accepts both Google issuer spellings; re-assert here so a
    # future library change cannot silently widen what we accept.
    if claims.get("iss") not in ("accounts.google.com", "https://accounts.google.com"):
        raise AuthError("unexpected issuer")

    return Identity(
        subject=subject,
        email=claims.get("email"),
        name=claims.get("name"),
        picture=claims.get("picture"),
    )
