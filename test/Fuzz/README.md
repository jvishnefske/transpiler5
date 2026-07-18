# Differential fuzzing harness

Adversarial, deterministic differential fuzzing for emitrust-cc: every
seed becomes a UB-free C11 program in the supported subset, compiled
both natively (clang) and through `emitrust-cc --emit=crate --build`,
with exit codes and stdout bytes compared byte-for-byte.

## Files

- `genprog.py` — pure seed → C program generator. Composes 2–5 feature
  templates (union puns, byte reinterprets, cell-slice globals, void*/
  member-base/null-ternary pointers, variadic + sprintf, StmtExprs,
  fn-ptr devirtualization, global-return chains, int-carrier/expect/
  missing-return) modeled directly on the test/EndToEnd/*.c executable
  spec. A configurable fraction of seeds (`--cross-fraction`, default
  0.5) is forced to combine ≥2 pointer-provenance templates.
- `differ.py` — one-iteration runner: `run_pair()` classifies PASS /
  UNSUPPORTED / MISCOMPILE / HARNESS_BUG. `--self-test` pushes a
  known-divergent hand-written pair through the comparison logic.
- `fuzz_differential.py` — campaign driver (thread pool, per-template
  accept-rate statistics, MISCOMPILE artifact bundles).
- `minimize.py` — grammar-level shrinker: delta-debugs the template
  instance list, then per-template value-pool choices, re-running the
  differ at each step.
- `fuzz-smoke.c` — lit smoke test (seeds 1–16, `REQUIRES: cargo`),
  part of `check-emitrust`.

## Running a big campaign

    nix develop -c python3 test/Fuzz/fuzz_differential.py \
        --emitrust-cc build/bin/emitrust-cc \
        --start 0 --count 5000 --jobs 8 --artifacts /tmp/fuzz-out

Useful flags: `--fail-on-miscompile` (CI mode), `--workdir DIR` (keep
scratch), `--keep-work` (keep per-seed build products), `--clang PATH`.
The driver probes once whether emitrust-cc's cargo child inherits
`CARGO_TARGET_DIR` and shares one target dir when it does; otherwise it
falls back to per-crate target dirs automatically.

## Determinism contract

A program is a pure function of `(seed, GENERATOR_VERSION)`: genprog
draws every choice from `random.Random(seed)` over explicitly ordered
lists — no wall clock, no `os.urandom`, no unsorted dict/set iteration.
The same seed on the same generator version yields a byte-identical
program, so any finding is reproducible from its seed number alone
(plus `--cross-fraction` if overridden). Bump `GENERATOR_VERSION`
whenever the seed→program mapping changes.

## Triage protocol for a MISCOMPILE

1. **Minimize**: shrink the diverging program at the grammar level:

       nix develop -c python3 test/Fuzz/minimize.py \
           --seed <seed> --emitrust-cc build/bin/emitrust-cc \
           --workdir /tmp/shrink-<seed> --output /tmp/min-<seed>.c

2. **Pin**: land the minimized program as a differential regression
   test `test/EndToEnd/<feature>-fuzz-<seed>.c` (same RUN-line pattern
   as the other EndToEnd tests: build crate, build native, diff).
3. **Fix** the compiler bug (a separate change; never adjust the
   generator to hide a divergence).
4. **Re-run** the seed range that found it, plus the smoke test, to
   confirm the fix and catch neighbors.

`HARNESS_BUG` means the native leg failed to compile or run: that is a
generator defect (UB or unsupported-by-clang output) and must be fixed
in genprog.py before its seed range is trusted.
