# The improvement loop (FR-63.6 continuous mode)

The loop is a Claude Code `/loop` (self-paced) that runs one iteration per
wake. The deterministic controller (`controller.py`) does the measuring,
gating, and ratcheting; a subagent (`OPTIMIZER.md`) does the editing. **No LLM
call is embedded in the Python** — the split is what makes the epoch comparison
valid.

## One iteration

```bash
export EMITRUST_CC=./build/bin/emitrust-cc      # or the nix-built binary

# 1. collect — the ranked queue over all three signals
python3 nix/harness/controller.py collect --explore <explore.json> ...

# 2. pick the top item (correctness before quality; long tail already excluded)
#    and verify its emitter site (file:line) yourself.

# 3. dispatch the optimizer subagent with that ONE item + the located site
#    (Agent tool; see OPTIMIZER.md). It edits the emitter + goldens, then stops.

# 4. gate → score → accept | revert, headless, correct exit codes:
python3 nix/harness/controller.py iterate --id 1 --note "<lint>: idiomatic form"
#    gate  = nix build + check-emitrust 100% + no new unsafe/allow
#    score = train warnings fell AND held-out not regressed
#    on any failure the controller reverts and exits non-zero.
```

`iterate` is the headless composition of `gate`, `score`, and
`accept`/`revert`. Run the steps individually when debugging.

## One-time setup (per epoch)

```bash
python3 nix/harness/controller.py freeze --id 1 --corpus test/EndToEnd --split
python3 nix/harness/controller.py establish --id 1   # champion train/held-out
```

`freeze` pins the corpus (file list + content hash) and splits it train/held-out
by seed; `establish` records the champion score every candidate is measured
against. Re-sampling the corpus is a NEW epoch — never compare totals across
epochs.

## Termination (stop conditions)

Stop the loop when ANY holds:

1. **Plateau / long tail** — `collect`'s top item is non-systematic (the queue
   is empty, or the top remaining clippy lint is below `--min-clippy` and every
   bug is fixed). A per-program lint is not worth an emitter change.
2. **Budget** — wall-clock / token ceiling.
3. **K dry rounds** — K consecutive iterations that reverted (no accept).
4. **Blocked** — the top item needs a design decision, an off-limits change, or
   a human-gated action (`git push`, `flake.nix`, external side effect). Record
   the blocker as an FR spike NO-GO and move to the next item.

## The trajectory

Every accepted iteration appends `(emitter_rev, metrics)` to
`nix/harness/ledger.json` under the epoch. Render the descent with:

```bash
python3 nix/harness/epoch.py ledger-show --id 1
```

The ledger is the loop's long-term memory: monotone clippy descent and zero
correctness regressions, auditable per revision.
