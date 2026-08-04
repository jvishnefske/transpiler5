# Optimizer subagent spec (FR-63.5)

The optimizer is the ONLY part of the loop that edits the emitter. It is a
Claude Code subagent dispatched by the loop (`LOOP.md`) with exactly one queue
item per iteration. It proposes an edit; it NEVER scores or gates its own work
— the deterministic controller (`controller.py`) does that. This separation is
load-bearing: it is why the epoch comparison between the champion and the
candidate is trustworthy.

## Input the subagent receives

- **One queue item** from `controller.py collect` — a single lint / bug /
  construct, with its `id`, `signal`, `kind`, `leverage`, and `example`.
- **The located emitter site** — a `file:line` anchor the dispatcher verified
  itself (usually `lib/Target/Rust/TranslateToRust.cpp` or a `lib/ImportC/*`
  lowering). The subagent confirms it before editing.
- **The train slice only** (`epoch-N.train.txt`). The held-out slice is never
  shown to the optimizer — that is the anti-Goodhart guard (FR-63.3).

## What the subagent does — one item, byte-diff-safe

1. **Locate & confirm.** Verify the anchor; find where the emitter produces the
   non-idiomatic spelling (or the bug / rejection).
2. **Test first.** Add or update a golden/EndToEnd test in the house style
   (intent comment naming the invariant it pins). For a quality lint the
   EndToEnd byte-diff already covers behaviour; pin the new *spelling* with a
   FileCheck golden. For a bug, add the minimal repro as an EndToEnd byte-diff.
3. **Edit the emitter** to emit the idiomatic form, gated on the EXACT
   condition the lint checks (e.g. fold `v = v + e` → `v += e` only when the
   LHS and the first operand are the same place and the op is commutative-safe
   for the type). Update every golden the new spelling shifts, IN THE SAME
   CHANGE.
4. **Hand back.** The subagent stops here. The controller builds, runs the
   byte-diff oracle, scores train+held-out, and accepts or reverts.

## Hard constraints (the subagent must not)

- **No new `unsafe`.** The `unsafe` count in emitted Rust is pinned at 0. If a
  change seems to need it, the change is wrong.
- **No new allow-attribute.** Do not silence a lint with `#[allow(...)]`; emit
  the idiomatic form. Only the established crate-root header
  (`#![allow(dead_code, unused_assignments)]`) is permitted, and the gate
  rejects any allow-line outside the champion's recorded set.
- **No behaviour change.** The emitted program must compute exactly what it did
  — the byte-diff oracle is the arbiter, and a "prettier" emission that shifts a
  byte of stdout is a miscompile, not a cleanup.
- **Off-limits, without a human + a new idea:** `clippy::needless_late_init`
  (a liveness change, not a spelling one) and cross-iteration loop liveness
  (miscompiled 3×). If the item touches these, record an FR spike NO-GO and
  stop — do not guess.
- **One item per iteration.** Do not opportunistically fix a second lint; it
  muddies the paired epoch comparison and the golden churn.

## Isolation (worktree waves)

When iterations run concurrently, isolate each in its own git worktree. **Never
`git stash` in an agent worktree** — the stash is shared across worktrees. Use
`diff` + `checkout` + `apply -3` to move a change between trees (CLAUDE.md).

## The handshake with the controller

```
loop: controller collect            → top item + located site
loop: dispatch optimizer subagent   → edits emitter + goldens (this spec)
loop: controller iterate --id N     → gate (build+byte-diff+scan) → score
                                       (train↓ & held-out not worse)
                                       → accept (ratchet+commit) | revert
```

The subagent's success is defined entirely by the controller's gate+score, not
by its own judgement. A rejected iteration is reverted and costs nothing but
time; a silently-wrong one cannot happen, because the byte-diff oracle sees it.
