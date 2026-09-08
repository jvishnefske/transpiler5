# emitrust web

The landing page and playground for `vishnefske.com`, plus the service that
backs them.

```
  browser ──► Traefik @ 192.168.1.185 ──► uvicorn @ 192.168.1.202:8080
                (TLS, CSP, rate limit)      (FastAPI + emitrust-cc)
```

## The one rule worth knowing

The free/paid boundary is enforced in exactly one place — `POST /api/compile`:

| request | requires |
| --- | --- |
| result already in the cache | nothing. No account, no meter. |
| result not in the cache | a verified Google identity **and** trial budget. |

The cache lookup happens **before** the auth check. That ordering is the whole
design: an anonymous visitor can drive every feature on the playground against
the prebaked examples and only meets the sign-in wall when they ask for
something genuinely new. It also means the trial meters *compiler work*, not
page views — and that a repeat of someone else's compile is free.

The client never decides this. `app.js` posts the compile and reacts to a 401;
a client-side "am I signed in" check would be both wrong (cached results need
no account) and forgeable.

## Layout

```
web/
  frontend/     index.html, playground.html, app.js, style.css  (no build step)
  server/       FastAPI app
    config.py     all env reading, in one place
    compiler.py   option WHITELIST + sandboxed emitrust-cc invocation
    cache.py      content-addressed results — also the authorization boundary
    auth.py       Google ID-token verification
    quota.py      the trial meter (SQLite)
    examples.py   the guided tour, warmed into the cache at startup
  examples/     the C sources behind that tour
  tests/smoke.py  runs the REAL compiler; see below
  deploy/       systemd unit + Traefik dynamic config
```

## Running it

```bash
# deps (not vendored). Inside the repo's nix devshell they are already
# present -- flake.nix carries them, so `nix develop -c ...` needs no venv.
pip install -r web/server/requirements.txt

# the transpiler links the nix devshell's LLVM/clang dylibs, so run inside it
nix develop -c python3 -m uvicorn web.server.main:app --port 8080 --reload
```

Then <http://127.0.0.1:8080>. With no `GOOGLE_CLIENT_ID` set the service still
boots and every cached example works; live compiles answer `503` with a plain
explanation rather than pretending to be an auth failure.

### Tests

```bash
nix develop -c python3 -m pytest web/tests -q   # the API layer + the meter
nix develop -c python3 web/tests/smoke.py       # the compiler wrapper
```

The pytest suite drives the real ASGI app in-process through
`httpx.ASGITransport` — no network, no live uvicorn — and pins the free/paid
boundary case by case: a cache hit is served to an anonymous caller without
touching the meter, a miss is 401/503/429 with the exact wording the user
sees, and a paid live compile deposits its result in the free lane so the next
anonymous request for the same source is a hit. Where a test needs a real
compile it runs the real `emitrust-cc` (and skips if the tree is not built).
The compiler is *forbidden* by default in those tests, so "no compile ran" is
checked rather than assumed.

One trap worth knowing before you add a test: `config.py` reads the
environment at IMPORT time, so `web/tests/conftest.py` sets `EMITRUST_*`
before the first `web.server` import. That is why the fixtures live there.

`smoke.py` runs the real compiler, not mocks, and that matters. It exists
because an `RLIMIT_NPROC` cap in the sandbox silently killed every
`--emit=crate` and `--recover` run while letting trivial ones through — MLIR
grows a verifier thread pool, and `RLIMIT_NPROC` is per-UID rather than
per-process. A test that only compiled `hello.c` would have passed. Process
count is now bounded by `TasksMax=` in the systemd unit, which is the correct
mechanism.

## Configuration

Everything is read once, in `config.py`. Nothing else touches `os.environ`.

| variable | default | notes |
| --- | --- | --- |
| `GOOGLE_CLIENT_ID` | *(empty)* | OAuth **web** client id. Public by design; served from `/api/health` so the frontend needs no build step. The client *secret* is never used — we verify ID tokens, we never exchange an auth code. |
| `EMITRUST_CC` | `build/tools/emitrust-cc` | the driver binary |
| `EMITRUST_NIX_DEVELOP` | *(empty)* | set to a `nix` path to wrap each invocation; leave empty when the service itself runs inside `nix develop` |
| `EMITRUST_STATE_DIR` | `/var/lib/emitrust-web` | cache + SQLite live here |
| `EMITRUST_TRIAL_DAILY` | `25` | live compiles per account per rolling day |
| `EMITRUST_TRIAL_LIFETIME` | `500` | lifetime backstop; `none` disables |
| `EMITRUST_TIMEOUT` | `20` | seconds per compile |
| `EMITRUST_CONCURRENCY` | `4` | concurrent live compiles. Each invocation is itself multi-threaded, so this multiplies rather than adds — keep it well under core count. |
| `EMITRUST_MAX_SOURCE` | `262144` | bytes |
| `EMITRUST_TRUST_PROXY` | `1` | **informational only** — nothing reads it. Proxy-header trust is uvicorn's `--proxy-headers` in the systemd unit. |

## Deploying to 192.168.1.202

```bash
sudo useradd --system --home /opt/emitrust --shell /usr/sbin/nologin emitrust
sudo install -d -o emitrust -g emitrust /opt/emitrust /var/lib/emitrust-web
sudo rsync -a --delete ./ /opt/emitrust/

printf 'GOOGLE_CLIENT_ID=%s\n' "$CLIENT_ID" | sudo tee /etc/emitrust-web.env
sudo chown root:emitrust /etc/emitrust-web.env && sudo chmod 640 /etc/emitrust-web.env

sudo cp web/deploy/emitrust-web.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now emitrust-web
curl -s localhost:8080/api/health | jq
```

On the Traefik host (`192.168.1.185`), drop `web/deploy/traefik-dynamic.yml`
into your dynamic config directory. It carries the router, the TLS resolver,
the CSP (Google Identity Services needs specific script/frame/connect
origins), and a coarse anonymous rate limit in front of the app's own
per-account meter.

In the Google Cloud console, the OAuth client's **Authorized JavaScript
origins** must list `https://vishnefske.com` — Google Identity Services checks
the origin, so a mismatch fails at the button, not at the API.

### First boot

Startup compiles all ten catalog examples into the cache (a few seconds). The
log line `example cache warm: N ready, M failed` is a real signal: a non-zero
`M` means the current build cannot handle one of the examples, and that
example silently falls into the paid lane until fixed.

## Threat model

The service runs a clang-based front end over source that arrives from the
internet. Be honest about what protects what:

- **The isolation boundary is the systemd sandbox** (`ProtectSystem=strict`,
  `PrivateTmp`, `SystemCallFilter`, `MemoryMax`, `TasksMax`, its own uid) — or
  a container/VM if you prefer. Put the service somewhere you would be willing
  to lose.
- **The in-process `rlimit`s are defense in depth**, not the boundary. They
  bound one invocation's CPU, address space and output size.
- **The option surface is a whitelist.** `emitrust-cc` inherits LLVM's global
  option registry — several hundred flags, some of which write files — so
  anything not explicitly listed in `compiler.py` is rejected by name. Never
  relax this into a blacklist.
- **Nothing is ever built or run.** The service only transpiles. `cargo build`
  on attacker-supplied source would execute build scripts, and running the
  emitted binary would execute attacker semantics. Neither is exposed, and
  neither is implemented.
- **Only ID tokens are trusted.** No client-supplied user id, email or
  "signed in" flag is ever believed.

`X-Forwarded-*` headers are trusted, so `:8080` must not be reachable from the
internet — only from Traefik.

## What is and is not verified

Verified against the real compiler (`web/tests/smoke.py`, all passing): the
option whitelist, argv construction, cache keying, the trial meter's
spend/refund/rollover, the sandbox limits, and every one of the ten catalog
examples compiling — including that the rejection example really does produce
a located `file:line:col` diagnostic and that `--recover` really does leave the
other functions intact.

Verified through the real ASGI app (`nix develop -c python3 -m pytest
web/tests`, 72 tests, all passing):

- **The free/paid boundary**, case by case — cache hit served anonymously with
  the meter never consulted; miss with no credential 401; miss with an invalid
  credential 401 (not 500); miss with a verified identity spends exactly one
  unit; exhaustion 429 while cached results keep working for that same user;
  and, with no `GOOGLE_CLIENT_ID`, 503 rather than a "sign in" that cannot
  work. Every rejection body is asserted verbatim.
- **The claim that a paid compile makes a result free for everyone** — a live
  compile through the real `emitrust-cc`, then the same source requested with
  no `Authorization` header at all, served from the cache with the compiler
  patched to fail loudly if it ran.
- **The refund policy** — a crash or a timeout hands the unit back and is not
  cached; an unsupported program does not, because a located rejection is the
  answer the user asked for, and it is cached so the next visitor gets it free.
  A failed cache write does not cost the user their answer as well as the
  deposit.
- **Routing** — the `StaticFiles` mount at `/` does not shadow `/api/*`, and
  `/`, `/playground` and the assets still serve.
- **Startup** — a missing compiler, a compiler that raises, and an unwritable
  state directory each degrade the service to "cached results only" instead of
  preventing boot.
- **`auth.verify` against the real google-auth**, including that a malformed
  credential is rejected without an outbound request, that a Google outage is
  503 rather than 401 or 500, and that the `google-auth[requests]` extra is
  actually installed.

Not verified:

- **The frontend at runtime.** `app.js` passes `node --check` and every
  element id it looks up exists in `playground.html`, but no browser has run
  it. The Google sign-in flow in particular has never executed against a real
  client id — the ID-token path is exercised only against forged and malformed
  tokens, never a genuine one.
- **Concurrency.** `MAX_CONCURRENT_COMPILES` and the trial meter's
  single-writer lock are argued, not measured under load.

Known gaps, by design or by deferral:

- The cache grows without bound and has no eviction.
- The trial meter keys on the Google account only. One person with several
  accounts gets several allowances.
