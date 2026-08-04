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
| `epoch-N.{train,heldout}.txt` | the deterministic split (optimizer sees train only) |
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
export EMITRUST_CC=./build/bin/emitrust-cc
python3 nix/harness/controller.py freeze --id 1 --corpus test/EndToEnd --split
python3 nix/harness/controller.py establish --id 1
python3 nix/harness/controller.py collect            # ranked queue
# ... optimizer subagent edits the emitter for the top item ...
python3 nix/harness/controller.py iterate --id 1 --note "assign_op_pattern"
python3 nix/harness/epoch.py ledger-show --id 1      # the descent
```

See `LOOP.md` for the full protocol and termination rules.
