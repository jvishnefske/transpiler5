# Working protocol: plan, spike, subagent TDD, commit, repeat

1. **Plan in design.md.** Every feature is a numbered FR with testable
   acceptance criteria; check its box only when tests validate it. Spike
   verdicts (GO / NO-GO / design constraints found) are recorded inside the
   FR entry so the rationale survives the session.
2. **Spike before implementing.** The business case is settled; the risk is
   implementation detail. De-risk with code experiments FIRST: adversarial
   inputs, differential probes (solo vs joint, mode A vs mode B), and
   round-trips (print -> parse, serialize -> reload). A spike that exposes a
   defect fixes it on the spot with a regression test. A NO-GO with measured
   blockers is a valid, recordable outcome (see the malloc-pool IR-rewrite
   NO-GO in design.md for the template).
3. **Subagent TDD.** Dispatch implementation to subagents with a precise,
   fact-grounded spec (file:line anchors you verified yourself). Tests are
   written first, in the house style: every test file opens with an intent
   comment saying what invariant it pins and why.
4. **Validate with the real oracles, then commit each increment.**

## Oracles (in order of authority)

- **EndToEnd byte-diff**: emitted crate's stdout `diff`ed against the
  clang-built native binary. This is THE correctness oracle. `cargo build`
  success is compile-only and CANNOT see a miscompile — never trust a
  codegen change on compile-clean evidence alone.
- **Full lit suite**: `nix develop -c ninja -C build check-emitrust`
  (includes EndToEnd, the c-testsuite ledger/ratchet, dialect round-trips,
  Driver goldens). Must be 100% before any commit.
- **Byte-identity invariants**: emitted symbol names are shared between the
  importer and the FR-40 item graph (CSymbolNaming.h); `--emit=crate` output
  is pinned byte-for-byte by golden tests. A "cleanup" that shifts a byte of
  emitted Rust is a behavior change and needs the full suite, not a unit run.

## Build

- `nix develop -c ninja -C build` (tools land in `build/bin/`).
- Adding a tool dir: `tools/<name>/CMakeLists.txt` + one line in
  `tools/CMakeLists.txt`, then `nix develop -c cmake build` to reconfigure.
- New lit-visible tools go in `test/lit.cfg.py` tools list AND
  `test/CMakeLists.txt` EMITRUST_TEST_DEPENDS.

## Repo-specific rules

- Rejection is a feature: unsupported constructs get LOCATED diagnostics,
  and recovery modes must never silently emit wrong code — a deferred or
  recovered item that reaches emission unresolved must fail loudly there
  (see `emitrust.extern_decl` / FR-52's marker contract).
- Dropping emitted code (dead stores, initializers) is legal only when the
  analysis PROVES no read on any path; the safe failure direction is a hard
  rustc error (E0381/E0384), never silent behavior change. Cross-iteration
  loop liveness was attempted three times and miscompiled — do not retry
  without a new idea AND the byte-diff suite in the loop.
- Worktree waves: never `git stash` in agent worktrees (the stash is shared
  across worktrees); use diff + checkout + apply -3.
