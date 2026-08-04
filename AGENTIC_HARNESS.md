# Agentic improvement harness (FR-63)

An autonomous loop that improves the C→Rust transpiler under a fixed safety
contract: **discovery tools generate a ranked backlog, a subagent implements one
item at a time, the byte-diff oracle and the ratchets gate every commit, and the
loop never ships red.** The agents are the actuators; the oracle is the
invariant; the ratchet files are the long-term memory. This is the canonical
harness doc — it wires the tools already in the tree (`nix/explore`,
`nix/clippy-eval`, `nix/corpus`, `test/RealWorld`, `test/CTestSuite`) into a
loop framed as an **optimization over a frozen epoch corpus**, and states what
the loop may and may not do on its own. The implementation lives in
`nix/harness/` (controller, epoch, signals) and is tracked as FR-63 in
`design.md`; see `nix/harness/README.md` for the file map.

The design's load-bearing idea is a **split** (§2c): the measure/gate/ratchet
layer is a deterministic Python controller, and a Claude Code subagent only
*proposes* an edit — it never scores or gates its own work. That separation is
why the paired comparison between the champion and a candidate emitter revision
over the frozen corpus is valid.

## 0. The contract (non-negotiable — every iteration obeys these)

- **The byte-diff oracle is supreme.** Correctness = emitted crate's stdout
  `diff`ed against the clang-native binary (`test/EndToEnd`, run by
  `check-emitrust`). `cargo build` clean and `clippy` clean are compile-only and
  CANNOT see a miscompile. No codegen change is trusted on compile-clean
  evidence alone.
- **Ratchets never regress.** Every metric with a committed baseline
  (`test/RealWorld/expected-transpile.txt`, the c-testsuite ledger,
  `nix/clippy-eval/clippy-baseline.json`, the kernel rejection tally) may only
  move in the improving direction; a regression fails the gate and the iteration
  reverts.
- **Loud rejection over silent miscompile.** A construct the harness cannot
  handle gets a LOCATED diagnostic or a hard rustc/panic error — never a quiet
  behaviour change. Dropping emitted code is legal only when analysis PROVES no
  read on any path.
- **No new `unsafe`; no new allow-attribute** beyond the established crate-root
  header (`#![allow(dead_code, unused_assignments)]`). The emitter must emit the
  idiomatic form, not suppress the lint. `unsafe` is pinned at 0. If a change
  seems to need `unsafe`, the change is wrong.
- **Held-out generalization.** The epoch corpus is split train/held-out by seed
  (§2b); the optimizer sees only train, the controller scores both, and a change
  that lowers train warnings while regressing held-out is rejected. This is the
  anti-Goodhart guard — it stops the optimizer special-casing the corpus it sees.
- **Off-limits without a human + a new idea:** cross-iteration loop liveness
  (miscompiled 3×), `needless_late_init` (a liveness change, not a spelling
  one). See CLAUDE.md and `nix/clippy-eval/LOOP.md`.
- **Human-gated actions:** `git push`, editing `flake.nix` (pre-commit
  guarded), any external/outward-facing side effect, and any change that needs a
  design decision (record it as an FR spike NO-GO instead of guessing).

## 1. Discovery — generate the backlog (parallel, cheap-first)

Each source emits a deduped, ranked, machine-readable signal. Run them in
parallel; they do not touch the tree.

| Source | Command | Signal |
|---|---|---|
| Flag/corpus explorer | `nix/explore/explore.py <corpus> [--byte-diff]` | CRASH, HANG, **MISCOMPILE** (bugs); distinct REJECT tags (demand/coverage) |
| Clippy quality loop | `nix/clippy-eval/clippy_eval.py` | ranked idiomatic-output debt (spelling lints) |
| RealWorld ratchet | `test/RealWorld/run_realworld.py` | per-program REJECTED/TRANSPILED + blocker tags (demand) |
| c-testsuite ledger | `test/CTestSuite` via `check-emitrust` | conformance ratchet |
| Embedded corpus | `nix build .#corpus-*` (`nix/corpus`) | external-validation demand (PARSE_FAIL vs REJECT) |

Cheapest-first: the explorer's transpile-only tier and clippy over EndToEnd are
seconds; the byte-diff / build tiers and the embedded corpus are minutes — run
them less often (nightly, or when the cheap tiers plateau).

## 2. Prioritize — one queue, scored

Merge the signals into a single ranked queue. Score each candidate by

    priority = severity × leverage × (1 / risk)

- **severity:** CRASH/HANG > MISCOMPILE > new REJECT tag (unsupported construct)
  > clippy spelling debt.
- **leverage:** how many items/warnings one fix retires — the FR-49 root-blame
  idea (credit a cascade to its cause, not its symptoms). A single emitter
  pattern trips hundreds of clippy warnings; a single missing construct blocks a
  whole family of corpus programs. Fix the root, not the leaf.
- **risk:** golden churn, blast radius, proximity to the off-limits list. A
  spelling change (stdout-preserving) is low risk; a liveness or ownership
  change is high.

Skip the long tail: per-program one-off rejections and 1-in-N corpus rarities
are not worth an emitter change (the explorer's plateau already declines to
chase them). Severity is a strict tier, not just a factor: a correctness bug
outranks a quality lint no matter how many warnings the lint retires, so the
queue is tiered by severity and ordered by the `leverage × 1/risk` product
*within* each tier (`nix/harness/signals.py`).

## 2a. The epoch — freeze once, compare in pairs

Sampling is redundant (CMSIS-DSP's 660 files are a few shapes), so the
discovery corpus is drawn once and **frozen** as `epoch-N`: the exact file list
plus a content hash (`nix/harness/epoch.py freeze`). Every emitter revision is
measured on the identical sample, so the warning delta between two revisions is
attributable to the emitter change alone — a paired comparison, and a single
systematic emitter fix retires hundreds of warnings at once. Re-sampling starts
a NEW epoch; totals are **never** compared across epochs (the population
changed). This is the epoch / compiler-revision optimization concept from
`~/src/dressage-design.md` §10–§11 made concrete.

## 2b. Held-out split — the anti-Goodhart guard

`epoch.py split` partitions the epoch train/held-out as a pure function of
`(seed, path)` — reproducible, and adding a file never reshuffles the rest. The
optimizer subagent is shown ONLY the train list (`epoch-N.train.txt`); the
controller scores both slices and rejects any change that regresses held-out
(`controller.py score`, exit 2). A "prettier" emission that overfits the visible
corpus fails here even when the byte-diff oracle passes.

## 2c. The controller / optimizer split (load-bearing)

The measure/gate/ratchet layer is a deterministic Python **controller**
(`nix/harness/controller.py`, no LLM) with subcommands
`collect · freeze · establish · gate · score · accept · revert · iterate`. The
editing is a Claude Code **optimizer** subagent (`nix/harness/OPTIMIZER.md`)
that changes ONE emitter site per iteration and hands back. The `champion.json`
records the current champion's train/held-out score and the permitted
allow-lines; `ledger.json` records `(emitter_rev, metrics)` per accepted
revision so the descent is queryable (`epoch.py ledger-show`). The gate enforces
the byte-diff oracle + no-new-unsafe/allow (contract clauses 1–3); score
enforces the ratchet + held-out (clauses 4–5).

## 3. The per-item loop (subagent-driven TDD)

For the top queue item:

1. **Spike / locate.** De-risk first (adversarial inputs, differential probes,
   round-trips). Find the emitter/importer origin as verified `file:line`
   anchors. A spike that finds a defect fixes it with a regression test; a
   measured NO-GO is a valid recorded outcome.
2. **Test first.** Write the failing test in the house style (intent comment
   naming the invariant it pins). For a bug: the minimal repro the explorer
   handed you, as an EndToEnd byte-diff or an Import golden. For a construct: the
   located-rejection or the new positive behaviour.
3. **Implement.** Dispatch ONE implementation subagent with a precise,
   fact-grounded spec (the anchors, the settled decisions, the oracle). For
   parallel emitter edits, isolate in worktrees and merge with the diff +
   checkout + apply-3 wave protocol (never `git stash` in a worktree — it is
   shared).
4. **Validate, in this order:**
   a. **Byte-diff oracle** — `check-emitrust` at 100% (the EndToEnd byte-diff is
      the correctness proof; update goldens the change legitimately shifts, in
      the same change).
   b. **Ratchets never-worse** — RealWorld, c-testsuite, kernel, clippy total.
   c. **The targeted metric improved** — re-measure the discovery signal.
5. **Commit or revert.** Green on all three → commit the increment, lower the
   relevant ratchet baseline, and record an FR in `design.md` if it is a
   feature (check its box only when tests validate it). Any regression → revert;
   the loop never ships red.

## 4. Orchestration & concurrency

- **Single self-paced loop** (`/loop`, or a cron via CronCreate): one iteration
  per wake — discover, pick, implement (subagent), validate, commit/revert,
  ratchet. Simplest; good default.
- **Fan-out workflow** for a discovery sweep or a batch of independent fixes:
  `Workflow` — run the discovery tools in parallel, dedup into the queue, then
  pipeline one subagent per independent item, each adversarially verified before
  it counts. Use worktree isolation when items edit the emitter concurrently.
- **Verification panel** for a claimed CRASH/MISCOMPILE before spending a fix on
  it: independent skeptics that try to REFUTE it (is it nondeterminism? is it
  defended-UB, i.e. a `PANIC` not a miscompile? does native actually define the
  behaviour?). The explorer's PANIC-vs-MISCOMPILE split is the first such check.

## 5. Stop conditions

- **Plateau** — the cheap discovery tiers stop producing new signatures and the
  ratchets stall (the explorer and clippy both report this). Stop; escalate to
  the expensive tiers or to a human for the next epic.
- **Budget** — wall-clock / token ceiling reached.
- **Blocked** — the top item needs a design decision, an off-limits change, or a
  human-gated action. Record the blocker (FR spike NO-GO with measured evidence)
  and move to the next item rather than guessing.

## 6. State, memory, and the dashboard

The harness's memory is on disk, not in a session: the ratchet baselines are the
never-worse floor, `design.md` is the rationale ledger (FR traceability, spike
verdicts), and `/memory` holds cross-session facts. A run reads them at start
and writes them at each commit, so the loop is resumable and auditable.

Track over time (one line per run): clippy total, RealWorld TRANSPILED count,
c-testsuite ledger, kernel rejection tally, explorer CRASH/MISCOMPILE count, and
the `unsafe` count (invariant: 0). A rise in any is a regression the ratchets
should already have caught; surfacing it here is the backstop.

## 7. Implementation status (FR-63, in `nix/harness/`)

Built and proven headless (see `design.md` FR-63):

- **FR-63.1 controller CLI** — `controller.py`; one iteration runs headless with
  Result-style exit codes; an injected gate failure reverts and exits non-zero.
- **FR-63.2 epoch freeze + ledger** — `epoch.py`, `ledger.json`; the pinned hash
  reproduces the sample; `ledger-show` renders the descent.
- **FR-63.3 held-out split** — `epoch.py split` + `clippy_eval.py --file-list`;
  a synthetic train-only overfit is rejected (score exit 2).
- **FR-63.4 signal merge + prioritizer** — `signals.py`; a crash outranks a
  clippy lint, the long tail is excluded, output is a stable ranked JSON queue.
- **FR-63.5 optimizer subagent** — `OPTIMIZER.md`; one item/iteration, byte-diff
  gated, goldens same change, no unsafe/allow, off-limits enforced, worktree
  isolation.
- **FR-63.6 continuous mode** — `LOOP.md`; a `/loop` wrapper stopping on plateau
  / long tail / budget / K dry rounds.

## 8. First epics (ready now)

1. **Clippy spelling debt** (`LOOP.md`): baseline 961 after loop #1
   (`println!`). Next stdout-preserving targets: `assign_op_pattern` (253),
   `explicit_auto_deref` (239) → ~469. Skip `needless_late_init` (off-limits).
2. **Explorer bug sweep**: point `explore.py --byte-diff` at `test/CTestSuite`
   and the `nix/corpus` sources (with their `--flags`) to hunt crashes and
   flag/input miscompiles the EndToEnd matrix does not exercise.
3. **Embedded demand**: once a libc sysroot is wired (`nix/corpus`), the
   ranked REJECT tags over CMSIS-DSP/lwIP/FreeRTOS become the FR backlog for the
   next construct-support epic — measured, not speculated (the Track-5
   discipline in `design.md`).
