# Working protocol: plan, spike, subagent TDD, commit, repeat

1. **Plan in design.md; queue in docs/plans/backlog.toml.** Every feature
   is a numbered FR with testable acceptance criteria in design.md (the
   prose EVIDENCE ledger — spike verdicts and measured evidence live
   there and nowhere else); check its box only when tests validate it.
   `docs/plans/backlog.toml` is the machine-readable index over the open
   items (status, rank, blocked_by). Query it instead of re-reading
   design.md:
   - `python3 docs/plans/plan.py next -n 3` — next unblocked items
   - `python3 docs/plans/plan.py brief FR-113` — just that prose entry
   - `python3 docs/plans/plan.py json | jq ...` — arbitrary queries
   Update BOTH files in the same commit (new FR → new index entry; box
   checked → status = landed). `plan.py check` enforces the sync and runs
   in the fast lit tier (test/Driver/backlog-check.c), so drift fails the
   pre-commit gate. When editing design.md by script, prefer anchored
   single-entry edits over start..end range replacements — a range edit
   once silently deleted three FR entries, and the checker exists because
   of it.
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

- **Meson is the build system** (FR-164, owner decision 2026-08-30 —
  "everything is meson forever"). `nix develop -c meson setup build`, then
  `nix develop -c meson compile -C build` (tools land in `build/tools/`).
  It links the monolithic libMLIR/libclang-cpp dylibs rather than
  per-component static archives, so there are no component link lists to
  keep in sync — the FR-163 class of defect cannot recur.
- `nix develop -c meson test -C build` runs the full lit suite and is the
  pre-commit gate. The suite is split into two complementary meson suites:
  `meson test --suite fast` runs everything except EndToEnd (~24% of the
  wall time — the inner-loop command), while plain `meson test` runs both
  tiers.
- The CMakeLists.txt files still exist but are NO LONGER CANONICAL and are
  not built by CI; they are scheduled for deletion (FR-164). Do not add new
  sources to them. The PDLL showcase (`EMITRUST_ENABLE_PDLL`) is CMake-only
  and dies with them unless someone ports it.
- Adding a tool dir: an `executable()` in `tools/meson.build`.
- New lit-visible tools go in the `test/lit.cfg.py` tools list.

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
- Spike/agent worktrees: run `scripts/spike-worktree-setup.sh` from inside
  the worktree FIRST. It fast-forwards to the main tree's HEAD (fresh
  worktrees have started 100+ commits stale) and pins llvm-config to the
  devshell's LLVM via a meson native file (the host's newer
  /usr/bin/llvm-config-N otherwise wins and the build fails on mixed
  headers). Compile in the background; measure unpatched behavior with
  read-only COPIES of the main tree's built tools meanwhile.
