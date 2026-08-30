# Improvement harness (FR-63)

An autonomous loop that improves the C→Rust emitter across three signals —
correctness bugs, emitted-Rust quality, and construct demand — under a fixed
safety contract. It turns the one-shot clippy ratchet (loop #1: 1621 → 961)
into a continuous optimization loop over a **frozen epoch corpus**, with the
byte-diff oracle as the hard gate and a held-out split as the anti-Goodhart
guard.

## The load-bearing split

- **Controller** (`controller.py`) — deterministic, scripted, no LLM. It
  measures, gates, ratchets, and commits. Because it is reproducible, the
  paired comparison between the champion and a candidate emitter revision is
  valid.
- **Optimizer** (`OPTIMIZER.md`) — a Claude Code subagent that edits ONE
  emitter site per iteration, byte-diff-safe, goldens in the same change. It
  proposes; it never scores or gates its own work.
- **Loop** (`LOOP.md`) — a `/loop` that runs `controller collect`, picks the
  top item, dispatches the optimizer, then `controller iterate`.

## Files

| file | role |
|---|---|
| `controller.py` | deterministic driver: `collect · freeze · establish · gate · score · accept · revert · iterate` |
| `epoch.py` | epoch freeze (file list + content hash), train/held-out split, trajectory ledger |
| `signals.py` | merge correctness / quality / demand into one ranked queue (`severity × leverage × 1/risk`, severity-tiered) |
| `epoch-N.json` | the frozen epoch: pinned files + `corpus_hash` |
| `epoch-N.exclude.txt` | paths omitted as UNMEASURABLE at the pinning rev, with reasons |
| `epoch-N.{train,heldout}.txt` | the deterministic split (optimizer sees train only) |
| `epoch-status.json` | which epochs are CLOSED history, when, why, and which files had drifted |
| `test_harness.py` | the frozen-population guards, gated by `test/Driver/harness-drift-guard.c` |
| `champion.json` | the current champion's train/held-out score + permitted allow-lines |
| `ledger.json` | per-epoch trajectory: `(emitter_rev, metrics)` for each accepted revision |
| `OPTIMIZER.md` / `LOOP.md` | the subagent spec and the loop protocol |

Sources it orchestrates (not replaced): `nix/clippy-eval/clippy_eval.py`
(quality), `nix/explore/explore.py --json` (correctness + demand),
`test/RealWorld/run_realworld.py` (demand), and the `check-emitrust` byte-diff
oracle.

## The contract (hard gates, never traded for score)

1. **Byte-diff oracle is supreme** — correctness is `check-emitrust` at 100%,
   never cargo/clippy clean (compile-only cannot see a miscompile).
2. **Full lit suite 100%**, goldens updated the same change a spelling shifts.
3. **No new `unsafe`; no new allow-attribute** beyond the crate-root header.
4. **Ratchets never regress** (clippy total, RealWorld, c-testsuite, kernel).
5. **Held-out generalization** — a train-only improvement that regresses
   held-out is rejected.
6. **Off-limits** without a human + a new idea: cross-iteration loop liveness,
   `needless_late_init`.
7. **Human-gated**: `git push`, `flake.nix`, external side effects, any design
   decision (record an FR spike NO-GO).

The gate (clauses 1–3) and score (clauses 4–5) enforce these mechanically; the
optimizer spec enforces 6–7.

## Quickstart

```bash
export EMITRUST_CC=./build/tools/emitrust-cc
python3 nix/harness/controller.py freeze --id 1 --corpus test/EndToEnd --split
python3 nix/harness/controller.py establish --id 1
python3 nix/harness/controller.py collect            # ranked queue
# ... optimizer subagent edits the emitter for the top item ...
python3 nix/harness/controller.py iterate --id 1 --note "assign_op_pattern"
python3 nix/harness/epoch.py ledger-show --id 1      # the descent
```

### Union corpora and measurability (epoch-4 and later)

`epoch.py freeze` takes repeatable `--corpus` and `--ext`, so one epoch can
pin the union of several roots and extensions:

```bash
python3 nix/harness/epoch.py freeze --id 5 \
    --corpus test/EndToEnd --ext .c --ext .cpp \
    --exclude nix/harness/epoch-5.exclude.txt
python3 nix/harness/epoch.py split --id 5 --seed 5 --held-out-frac 0.25
```

Only **measurable** files may be pinned: the file must transpile to a crate
(`emitrust-cc --emit=crate`) and `cargo clippy` on that crate must yield a
COMPLETE tally — it may fail on a deny-by-default clippy lint (that failure
*is* the tally), but not on a rustc error, which truncates the count and
leaves the metric undefined. `clippy_eval` silently skips an unmeasurable
file, so pinning one would only pad the denominator. Measurability is a
property of the pinning revision; `--exclude` records the omissions as
provenance, and a file that starts transpiling later does **not** join the
epoch (that is a new population, i.e. a new epoch).

Defaults reproduce the epoch-1..3 shape exactly (one root, `.c`,
non-recursive), so those frozen documents are never rewritten.

### The drift guard and closed epochs (FR-144)

The paired comparison is only valid while the pinned files still hold the
pinned bytes. Until FR-144 nothing checked that: `ledger_append` compared the
epoch *document*'s stored hash against the ledger's, never against the files
on disk, and nothing called `verify` automatically — so epochs 1, 2 and 3 all
drifted unnoticed and every trajectory recorded against them was measured
over a population that had already moved. Now `epoch.assert_comparable`
re-hashes the population and **refuses** (non-zero) on drift, and
`ledger_append`, `controller establish/score` and the clippy ratchet all go
through it.

Drift is **not** repaired by re-freezing — that would rewrite the document
the ledger's numbers were measured against. An epoch is instead *closed*:

```bash
python3 nix/harness/epoch.py close --id 3 --reason "drifted: ..."
python3 nix/harness/epoch.py status     # LIVE / CLOSED per epoch
```

`close` writes only `epoch-status.json` (when, at which rev, why, and each
drifted file with its blame); the frozen `epoch-N.json` is left byte for byte
alone. A closed epoch fails `verify` and is refused by every comparison, so
its numbers can be read but never extended. Epochs 1–4 are closed history —
their ledger trajectories are real measurements of a moving population, which
is exactly why nothing may extend them. Epoch-4 is the mechanism working as
designed rather than a defect: FR-61f-c legitimately edited pinned
`test/EndToEnd/deferred-mut-slice-index.c` on 2026-08-29, `verify --id 4`
went red, and the response was to freeze **epoch-5** (the same union corpus,
measurability re-probed at `f734fed`: 244 files, the same 20 exclusions) and
close epoch-4 — never to re-freeze it or `--update` its baseline. Epoch-5's
numbers are therefore **not** a continuation of epoch-4's: the population
changed, so the two totals are not a trajectory.

The committed clippy ratchet is likewise pinned: `clippy-baseline-epoch5.json`
carries `epoch_id` + `corpus_hash`, the default `clippy_eval.py` invocation
measures **the epoch's file list** (not whatever `.c` is in the directory),
and an unpinned baseline is refused. The three consumers of "which baseline is
authoritative" — `clippy_eval.DEFAULT_BASELINE`, `signals.DEFAULT_CLIPPY` and
`controller.CLIPPY_BASELINE` (which `--update`s and *commits* it) — must name
the same file; `test_harness.py` fails if they diverge.

See `LOOP.md` for the full protocol and termination rules.
