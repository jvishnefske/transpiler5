"""auth.verify() itself, against the real google-auth library.

INTENT: the boundary tests in test_compile_boundary.py patch auth.verify so
they can talk about identities without minting Google tokens. That is the
right trade for those tests and the wrong one for this file: if verify() were
only ever mocked, a wiring break (a missing dependency, a library that raises
a class we do not catch) would ship silently and every live compile would
answer 500 or 503.

So this file drives the REAL function.

Two failures it exists to prevent, both measured on this host:

  1. `google.auth.transport.requests` imports `requests` lazily and raises
     ImportError without it -- the `google-auth[requests]` extra. `import
     google.auth` succeeds regardless, so the gap is invisible until a token
     is actually verified. Untreated, the service answers 503 "google-auth is
     not installed on the server" for every live compile, on a box where
     google-auth *is* installed.
  2. verify_oauth2_token does not raise only ValueError: a wrong issuer is a
     google.auth.exceptions.GoogleAuthError and an unreachable Google is a
     TransportError. Neither is a ValueError, so both used to escape
     auth.verify() uncaught and become a 500 on the compile endpoint. A user
     with a stale token must get 401; a Google outage must get 503.

Run:  nix develop -c python3 -m pytest web/tests -q
"""

from __future__ import annotations

import pytest

from web.server import auth, config

from conftest import TEST_CLIENT_ID

# Well-formed compact JWS -- three base64url segments -- but signed by nobody.
# Structurally valid, so it gets past the cheap pre-check and exercises the
# real google-auth path.
FORGED_TOKEN = (
    "eyJhbGciOiJSUzI1NiIsImtpZCI6ImRlYWRiZWVmIiwidHlwIjoiSldUIn0"
    ".eyJpc3MiOiJodHRwczovL2FjY291bnRzLmdvb2dsZS5jb20iLCJzdWIiOiIxIn0"
    ".AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
)


@pytest.fixture
def live_lane(monkeypatch):
    monkeypatch.setattr(config, "GOOGLE_CLIENT_ID", TEST_CLIENT_ID)


def test_no_client_id_is_unavailable_not_an_auth_error(monkeypatch):
    """The 503-vs-401 distinction starts here, in the exception type."""
    monkeypatch.setattr(config, "GOOGLE_CLIENT_ID", "")
    assert auth.enabled() is False
    with pytest.raises(auth.AuthUnavailable) as exc:
        auth.verify("anything")
    assert str(exc.value) == (
        "This deployment has no Google client id configured, so live "
        "compiles are disabled. Cached results still work."
    )


@pytest.mark.parametrize("token", [None, ""])
def test_absent_credential_is_an_auth_error(live_lane, token):
    with pytest.raises(auth.AuthError) as exc:
        auth.verify(token)
    assert str(exc.value) == "no credential supplied"


@pytest.mark.parametrize(
    "token",
    [
        "not-a-jwt",
        "only.two",
        "four.parts.are.wrong",
        "has spaces.in.it",
        "..",
        "a." * 3,
        "x" * 9000,  # past MAX_CREDENTIAL_BYTES: never reaches a parser
    ],
)
def test_structurally_malformed_credentials_are_rejected_offline(
    live_lane, token, monkeypatch
):
    """AuthError, with no outbound request -- enforced by a tripwire.

    Rejecting garbage before the cert fetch matters twice over: an unverified
    caller must not be able to make our server issue an HTTPS request to
    Google (one POST per junk string is a free amplifier), and token rejection
    must not depend on Google being reachable.
    """
    from google.oauth2 import id_token as google_id_token

    def _tripwire(*args, **kwargs):
        raise AssertionError(
            "structurally invalid credential reached google-auth (and the "
            "network) instead of being rejected up front"
        )

    monkeypatch.setattr(google_id_token, "verify_oauth2_token", _tripwire)
    with pytest.raises(auth.AuthError) as exc:
        auth.verify(token)
    assert str(exc.value) == "credential is not a JWT"


def test_the_cheap_precheck_does_not_reject_a_wellformed_token(live_lane):
    """The pre-check must be structural only -- it must never be the verifier.

    If it rejected FORGED_TOKEN, the real signature check below would never
    run and this file would be testing a regex.
    """
    assert auth._looks_like_a_jwt(FORGED_TOKEN) is True


def test_real_verify_rejects_a_forged_token(live_lane):
    """The full google-auth path: signature/kid checking really happens.

    Needs to reach Google's cert endpoint; if it cannot, our code turns that
    into AuthUnavailable (a 503, not a 500) and the test skips rather than
    reporting a failure it cannot distinguish from an outage.
    """
    try:
        with pytest.raises(auth.AuthError) as exc:
            auth.verify(FORGED_TOKEN)
    except auth.AuthUnavailable as unreachable:
        pytest.skip(f"google's certs are unreachable: {unreachable}")
    # Not a structural complaint -- google-auth got far enough to look for the
    # signing key, which is what proves the wiring is real.
    assert "not a jwt" not in str(exc.value).lower()


def test_verify_reports_a_google_outage_as_unavailable(live_lane, monkeypatch):
    """TransportError is OUR problem: 503, not 401 and not 500.

    Telling a signed-in user their credential is bad because we could not
    reach Google would be a lie, and a 500 would be an unhandled crash on a
    path that faces the internet.
    """
    from google.auth import exceptions as google_exceptions
    from google.oauth2 import id_token as google_id_token

    def _boom(*args, **kwargs):
        raise google_exceptions.TransportError("connection refused")

    monkeypatch.setattr(google_id_token, "verify_oauth2_token", _boom)
    with pytest.raises(auth.AuthUnavailable) as exc:
        auth.verify(FORGED_TOKEN)
    assert "connection refused" in str(exc.value)


def test_verify_reports_a_wrong_issuer_as_an_auth_error(live_lane, monkeypatch):
    """google-auth raises GoogleAuthError, not ValueError, for a bad issuer."""
    from google.auth import exceptions as google_exceptions
    from google.oauth2 import id_token as google_id_token

    def _wrong_issuer(*args, **kwargs):
        raise google_exceptions.GoogleAuthError("Wrong issuer.")

    monkeypatch.setattr(google_id_token, "verify_oauth2_token", _wrong_issuer)
    with pytest.raises(auth.AuthError) as exc:
        auth.verify(FORGED_TOKEN)
    assert "Wrong issuer." in str(exc.value)


def test_verify_rejects_a_token_with_no_subject(live_lane, monkeypatch):
    from google.oauth2 import id_token as google_id_token

    monkeypatch.setattr(
        google_id_token,
        "verify_oauth2_token",
        lambda *a, **k: {"iss": "https://accounts.google.com", "email": "x@y"},
    )
    with pytest.raises(auth.AuthError) as exc:
        auth.verify(FORGED_TOKEN)
    assert str(exc.value) == "token carries no subject"


def test_verify_re_asserts_the_issuer_itself(live_lane, monkeypatch):
    """Our own issuer check, independent of the library's."""
    from google.oauth2 import id_token as google_id_token

    monkeypatch.setattr(
        google_id_token,
        "verify_oauth2_token",
        lambda *a, **k: {"iss": "https://evil.example", "sub": "1"},
    )
    with pytest.raises(auth.AuthError) as exc:
        auth.verify(FORGED_TOKEN)
    assert str(exc.value) == "unexpected issuer"


def test_verify_returns_the_identity_it_was_given(live_lane, monkeypatch):
    from google.oauth2 import id_token as google_id_token

    monkeypatch.setattr(
        google_id_token,
        "verify_oauth2_token",
        lambda *a, **k: {
            "iss": "accounts.google.com",
            "sub": "108000000000000000001",
            "email": "user@example.com",
            "name": "A User",
            "picture": "https://lh3.googleusercontent.com/x",
        },
    )
    identity = auth.verify(FORGED_TOKEN)
    assert identity.subject == "108000000000000000001"
    assert identity.to_json() == {
        "email": "user@example.com",
        "name": "A User",
        "picture": "https://lh3.googleusercontent.com/x",
    }
    # The subject -- not the email -- is the trial key: emails are mutable and
    # reusable, `sub` is not.
    assert "sub" not in identity.to_json()


def test_the_requests_transport_is_actually_installed():
    """The google-auth[requests] extra. Its absence is invisible until a
    token is verified, and then it is a 503 on every live compile."""
    from google.auth.transport import requests as google_requests

    assert google_requests.Request is not None
