## Validation

Every requirement above is validated exclusively by regression tests run by
the lit suite; no requirement is checked off based on manual inspection.
The suite must pass warning-free in the pinned dev shell before a
requirement's box may be ticked. CI (.github/workflows/ci.yml, rewritten by
FR-246) enforces the same gate on every push and pull request in the pinned
Nix dev shell: the fast lit tier first (which includes all three conformance
ledgers with their two-way ratchets — any MISCOMPILE or manifest drift fails
the run), then the EndToEnd byte-diff tier, with the ledger reports surfaced
in the run summary via the FR-243 side-channel. The clippy and binding
ratchets run weekly and on demand in .github/workflows/ratchets.yml.

