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


def verify(id_token_str: str | None) -> Identity:
    """Verify a Google ID token. Raises AuthError / AuthUnavailable."""
    if not enabled():
        raise AuthUnavailable(
            "This deployment has no Google client id configured, so live "
            "compiles are disabled. Cached results still work."
        )
    if not id_token_str:
        raise AuthError("no credential supplied")

    try:
        from google.auth.transport import requests as google_requests
        from google.oauth2 import id_token as google_id_token
    except ImportError as exc:  # pragma: no cover - deployment error
        raise AuthUnavailable(
            "google-auth is not installed on the server"
        ) from exc

    try:
        claims = google_id_token.verify_oauth2_token(
            id_token_str,
            google_requests.Request(),
            config.GOOGLE_CLIENT_ID,
        )
    except ValueError as exc:
        raise AuthError(str(exc)) from exc

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
