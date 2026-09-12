# Working protocol: measure the goal, spike, subagent TDD, commit, repeat

## 0. THE GATE IS NOT THE GOAL

The gate (lit suite, ratchets) proves you did not break anything. **TRACTOR
PASS proves you moved.** They are not the same number and they can move
independently: one session took the gate 997 → 1040 across 21 FRs and 43 new
tests while TRACTOR moved **40 → 41**.

Before ranking any work, read the current census
(`scripts/tractor-census.py`, and FR-223 for what it found). Before claiming
a change is worth N cases, measure N with the three-stage harness. An
emit-stage win with +0 PASS is a legitimate and publishable result — say so
explicitly rather than letting a reader infer yield.

## 1. Plan in the ledger; queue in docs/plans/backlog.toml

Every feature is a numbered FR with testable acceptance criteria in the
prose EVIDENCE ledger — spike verdicts and measured evidence live there and
nowhere else. Since FR-242 the ledger is SPLIT: design.md is only the
preamble; every entry is its own file `docs/plans/entries/<ID>.md` (its
checkbox is line 1), frozen chapters live in `docs/design/`, and
`docs/plans/ledger.manifest` lists all members in original byte-stream
order — `plan.py join` prints the joined ledger, byte-identical to the
old monolith. `docs/plans/backlog.toml` is the machine-readable index
(status, rank, blocked_by). Query, don't read:

- `python3 docs/plans/plan.py next -n 3` — next unblocked items
- `python3 docs/plans/plan.py brief FR-113` — just that prose entry
- `python3 docs/plans/plan.py json | jq ...` — arbitrary queries
- `python3 docs/plans/plan.py join | grep ...` — whole-ledger search

**Edit one entry file; never range-edit across entries** (a range edit on
the old monolith once silently deleted three FR entries — the split exists
to make that impossible). NEW ENTRY, one commit: create
`docs/plans/entries/FR-N.md` (line 1 = its `- [ ] FR-N ...` box), add its
path to ledger.manifest right after the newest existing FR entry's line,
and add the backlog.toml item. Check a box (line 1 of the entry file) only
when tests validate it, flipping the backlog status in the same commit.
`plan.py check` enforces the two-way sync, manifest hygiene, and per-file
anchor resolution, and runs in the fast lit tier, so drift fails the
pre-commit gate.

MERGE REPLAY for a branch that still edits the pre-split design.md:
`python3 docs/plans/plan.py join > design.md`, apply their design.md
patch, then `python3 scripts/split-ledger.py --write`.

**Treat an existing entry's prose as a HYPOTHESIS, not a finding.** In one
session, six of eight increments corrected the entry that launched them, and
two corrected entries that had themselves been written to warn about the
error being made. Entries are reliable about what was MEASURED and unreliable
about WHY.

## 2. Spike before implementing

De-risk with code experiments FIRST: adversarial inputs, differential probes,
round-trips. A spike that exposes a defect fixes it on the spot with a
regression test. **A NO-GO with measured blockers is a valid, recordable
outcome** (see the malloc-pool IR-rewrite NO-GO for the template; FR-178 and
FR-221 are two of the more valuable entries in the ledger and both are NO-GOs).

**Write the spec's hypothesis as a LEAD, with permission to refute, and say
that contradiction is the wanted outcome.** This is the single highest-yield
habit in the protocol — it is what caught every error listed in §3.

## 3. COUNTING RULES — every one of these has cost a wrong decision

**Count PROGRAMS, not EVENTS, not SITES.** A first-failure histogram has
produced a wrong yield **nine** times (FR-61f ×2, FR-165, FR-138, FR-177,
FR-178, FR-221, FR-223, FR-224). FR-221 cited the previous six and then ranked
"83 of 202 EMIT_FAIL cases, 33% of the corpus" — which was 83 *events* over
**three source programs and four sites**, 80 of them one file built 128 ways;
deleting the whole check measured **+0**. **FR-224 then committed the trap
inside the entry written to describe it**, projecting +14 from a blocker count
without checking `kind` — 9 of the 13 were `lib` and needed exports, so the
honest figure was +4 to +6. Assume you are about to do this too.

**Marginal yield is a property of the SET.** A blocker in 83 cases is worth
zero if all 83 also need four other fixes. Use the census's set-cover, not
the frequency column.

**A STRICT-MODE ERROR IS A FIRST-FAILURE READING OF ONE CASE.** `emitrust-cc`
halts at the first error, so its output tells you a case's FIRST blocker and
nothing about the rest. Never read a blocker set from strict output — and
agreement across cases is not corroboration: twenty cases reporting the same
first error are twenty first failures. Re-run with `--recover` and **count the
`unimplemented!` stubs**, which is what FR-226 should have done (it read
"exactly one error" off twenty strict runs; recovery showed 14 stubs across
five blockers, and the entry had to be corrected within the hour).

**Clearing every blocker is required, not just the ones your symbol needs.**
`tractor-eval.py` is STRICT MODE ONLY, never `--recover`: a crate whose
thirteen unrelated functions are `unimplemented!()` scores zero even when the
one dlsym'd symbol works.

**Clearing a stage is not passing.** The rubric is emit → `cargo build` →
`dlopen`+`dlsym`, with NO partial credit. A `lib` case that clears emit still
needs an exportable symbol. Always report which stage a number refers to.

**A zero measured over one corpus is a claim about that corpus.**
`test/EndToEnd` is curated hand-written tests that by construction call
everything they define; c-testsuite is conformance code; RealWorld is real.
They disagree. FR-220 measured "zero dead functions" over the epoch and built
a plan on it that c-testsuite falsified by 10 and RealWorld falsified
outright.

**Raw-to-distinct fan-out is ~5–8×.** Anyone reading raw occurrence counts is
roughly that factor wrong.

## 4. Subagent TDD

Dispatch to subagents with a precise, fact-grounded spec (file:line anchors
**you ran the tool to confirm, not ones you copied from a spike or an FR
entry**). This is the single most repeated mistake in this repo's specs: one
anchor had shifted 300 lines, and FR-229's two waves BOTH carried a wrong one
— in each case the named line existed but the case never reached it, because
an earlier rejection fired first. A plausible anchor that is never executed
costs the agent an hour. Confirm by running `emitrust-cc` on the actual input
and reading which diagnostic fires. Tests are written first, in the house style: every test
file opens with an intent comment saying what invariant it pins and why.

Tell the agent: **implement first, sweep the corpus afterwards.** Six agents
died to a stream watchdog in one session, every one during a long
uninterrupted analysis stretch, and a long sweep with nothing yet written to
the worktree loses the most when it dies.

Tell the agent the bar: **make it work; a LOCATED rejection is an acceptable
floor; silently wrong or silently unbuildable is not.** And tell it to **stop
and report** rather than half-fix or weaken an oracle. Agents that stopped
produced the two most valuable results of the session.

## 5. Validate with the real oracles, then commit each increment

### Oracles, in order of authority

- **EndToEnd byte-diff** — emitted crate's stdout `diff`ed against the
  clang-built native. THE correctness oracle. `cargo build` success is
  compile-only and CANNOT see a miscompile.
- **Full lit suite** — `nix develop -c meson test -C build`. 100% before any
  commit.
- **Byte-identity invariants** — `--emit=crate` output is pinned
  byte-for-byte. A "cleanup" that shifts a byte of emitted Rust is a behavior
  change and needs the full suite.
- **Ratchets** — clippy at its epoch pin, the binding metric, CTestSuite and
  Cpp17Suite. All strict in BOTH directions: an unrecorded new PASS fails too.

### What the byte-diff oracle CANNOT see

It is the highest authority and it still has blind spots. Do not treat a green
byte-diff as proof of soundness for these classes:

- **Uninitialized memory.** Every corpus harness initializes every field, so
  `&mut T` over a legally-uninitialized C object is invisible to it (FR-212).
- **Dead code and unused items.** Nothing executes them.
- **Drop timing** where no destructor is observable.
- **UB in the C input.** Two compilers may legally differ; check
  well-definedness FIRST, or you will "fix" a program that has no defined
  answer.

### Golden movement

- Measure with a paired `--emit=rust` sweep before and after, and report the
  count.
- **Distinguish "moved" from "rejected by both sides."** A naive sweep counts
  files both tools reject as moved.
- **A hash sweep is BLIND to `split-file` sub-units** — one reported 0 of 992
  changed while six lit tests flipped. Run the full lit suite too; "zero
  golden churn" from a sweep alone is not a claim you may make.
- Re-verify every moved golden by its own oracle. Never re-bless.

## Build

- **Meson is the build system.** `nix develop -c meson setup build`, then
  `nix develop -c meson compile -C build` (tools land in `build/tools/`).
- `nix develop -c meson test -C build` is the pre-commit gate.
  `meson test --suite fast` (everything except EndToEnd, ~24% of wall time)
  is the inner loop.
- CMakeLists.txt files are NO LONGER CANONICAL and are scheduled for deletion
  (FR-164). Do not add sources to them.
- Adding a tool dir: an `executable()` in `tools/meson.build`. New
  lit-visible tools go in `test/lit.cfg.py`.

## Measuring the goal (TRACTOR)

    nix develop -c python3 scripts/tractor-eval.py /home/j/PUBLIC-Test-Corpus \
      --emitrust-cc build/tools/emitrust-cc --out <PRIVATE dir> --no-diffs -j 6

- **`--out` defaults to a SHARED path.** Two agents overwrote each other and
  one reported a number produced by another agent's binary. Always pass a
  private `--out`, and verify `emit_cmd[0]` is your own build.
- **Do NOT pass `--extra-cflags`.** The openssl recipe that used to be
  documented BREAKS the cmake configure step: all 202 EMIT_FAIL cases then
  report one `cmake configure failed` instead of their real blocker,
  destroying the census while leaving PASS unchanged.
- `scripts/tractor-census.py` gives per-case blocker sets and set-cover. Use
  it to rank work; use `tractor-eval.py` to score it. **Every set it reports
  is a LOWER BOUND and every yield an UPPER BOUND, with no exceptions** — the
  importer bails at the first error inside any item it rejects, and every
  non-passing case has at least one rejected item. It once printed a
  "CONFIRMED" column claiming otherwise; that column was ~92% wrong and has
  been removed. **Its one calibration point: it predicted +14 EMIT for the
  system-header fix and FR-224 delivered +7.** Halve its figures until there
  are more data points.
- **A corpus-runner early exit silently degrades PASS while leaving EMIT
  intact.** One run reported `NOT_DISCOVERED 47 / PASS 6` under machine
  contention while its `emitted 57/252` line was correct. Before trusting any
  PASS number, check the `corpus runner exited` line and confirm
  `discovered_by_corpus_runner` matches the emitted count.
- **A refusal's wording is part of the ranking instrument.** The census keys
  on the diagnostic, so giving a rejection bespoke prose moves it out of its
  `libc:<name>`-style tag and into the `other` junk bucket, where a future
  reader ranking work will not see it. A new refusal class needs a
  `RejectionLedger` needle in the same commit.

## Repo-specific rules

- **Rejection is a feature**: unsupported constructs get LOCATED diagnostics,
  and recovery must never silently emit wrong code — a deferred or recovered
  item reaching emission unresolved must fail loudly (FR-52's marker
  contract).
- **Dropping emitted code** (dead stores, initializers) is legal only when
  the analysis PROVES no read on any path; the safe failure direction is a
  hard rustc error (E0381/E0384), never silent behavior change.
  Cross-iteration loop liveness was attempted three times and miscompiled —
  do not retry without a new idea AND the byte-diff suite in the loop.
- **Recovery structurally manufactures dead functions.** It drops a rejected
  CALLER, which orphans every function only that caller reached. Any rule
  that must hold for all emitted code has to survive `--recover`, or it
  breaks FR-44's guarantee that a partially ported project still compiles.
- **`unsafe` is scored.** The rubric requires zero. A binding, shim, or export
  that relocates `unsafe` rather than discharging its obligation is not a
  solution (FR-224).
- **Epoch discipline.** The clippy and binding baselines pin a FROZEN file
  list. Editing a pinned EndToEnd source CLOSES the epoch. Adding a new test
  does not. Across an epoch boundary the totals are NOT a trajectory — the
  population changed. THREE consumers name the baseline document
  (`clippy_eval.py`, `nix/harness/controller.py`, `nix/harness/signals.py`);
  if they diverge, the controller commits a file the ratchet never reads.
- **Worktree waves**: never `git stash` in agent worktrees (the stash is
  shared); use diff + checkout + apply -3. If an agent's diff is cumulative
  over a patch you prepared, the 3-way will conflict — copy the changed files
  straight from its worktree instead.
- **Spike/agent worktrees**: run `scripts/spike-worktree-setup.sh` **TWICE**.
  It rewrites itself mid-run, so the first invocation writes a corrupt
  `build-native.ini`; the second, with HEAD already correct, produces a clean
  pin. Then `git submodule update --init third_party/c-testsuite
  third_party/llvm-test-suite` (add `--reference /home/j/transpiler5` to
  dedupe the ~2G llvm-test-suite checkout), or the CTestSuite/CppStdSuite
  ratchets fail on that alone.
- **`/tmp` is tmpfs**, not on `/`. Scratch there costs RAM, not disk. Disk
  pressure comes from `/nix/store` and `~/.cache`; each agent worktree build
  is ~475M.
- **Concurrency**: max 3 agents. Do not start a build below ~11G free.
- **The Bash cwd persists across calls, and a `cd` into an agent worktree will
  silently retarget your next `git commit`, `meson test -C build`, and every
  relative path you edit.** This happened: an FR entry was written into an
  agent's worktree and committed on its branch. It was harmless only because
  the agent had not started editing yet. Start commands that touch the main
  tree with `cd /home/j/transpiler5 &&`, and check the testlog path meson
  prints — it names the tree the run actually used.
