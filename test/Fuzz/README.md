# Differential fuzzing harness

Adversarial, deterministic differential fuzzing for emitrust-cc: every
seed becomes a UB-free C11 program in the supported subset, compiled
both natively (clang) and through `emitrust-cc --emit=crate --build`,
with exit codes and stdout bytes compared byte-for-byte — **three
ways**: the generator's own expected-output oracle vs the native binary
vs the transpiled binary.

## Three-way oracle

genprog.py knows each program's semantics, so every template has an
exact Python evaluator (`evaluate_plan`) that reproduces the program's
stdout bytes and exit code with explicit wrapping arithmetic: 32-bit
two's-complement int/unsigned, 64-bit for the carrier's `unsigned
long`, little-endian byte puns (asserted at import — both legs run on
the same little-endian host). Classification:

- `PASS` — oracle == native == transpiled.
- `MISCOMPILE` — transpiled differs while native matches the oracle,
  so the verdict rests on two independent witnesses.
- `GENERATOR_ORACLE_BUG` — native disagrees with the oracle: the
  generator emitted UB, the evaluator drifted from the renderer, or
  clang surprised us. Hard-fails the run like HARNESS_BUG.
- `UNSUPPORTED` — unchanged (emitrust-cc rejected the program).

**Oracle exemptions: none.** All ten templates are deterministic
integer/byte/string arithmetic and are simulated exactly, including the
sprintf format matrix (%d/%u/%x/%c/%s with widths and zero-pad, which
Python's %-formatting reproduces for these argument ranges).

## Files

- `genprog.py` — pure seed → C program generator. Composes 2–5 feature
  templates (union puns — optionally declaring an equal-width byte-array
  arm accessed only through the integer arm, byte reinterprets,
  cell-slice globals, void*/member-base/null-ternary pointers, variadic
  + sprintf, StmtExprs, fn-ptr devirtualization — optionally through a
  local void* holder with cast-calls, global-return chains,
  int-carrier/expect/missing-return, writeback ordering with RHS/index
  calls mutating a distinct subobject of the assigned global, C99-45
  bit-fields with mixed runs and sign/zero extension, C99-48 FILE*
  round-trip I/O, C99-13 compound literals in expression position with
  the loop re-zero rule, C99-4/28 signed plain-char semantics with the
  full escape set and string-literal expression reads, the C99-48
  curated libc subset (string.h/memory ops, abs, atoi, IEEE-exact math
  observed via %f on exactly-representable values; exit() is omitted so
  the digest chain never truncates), and C99-44 float puns observed
  exclusively through their BITS via integer arms — never %f — with all
  values in the normal range, so struct.pack keeps the evaluator exact
  and no x87 excess-precision concern arises on the SSE native leg)
  modeled directly on the test/EndToEnd/*.c executable spec.

  printf format coverage: the sprintf matrix includes the C99-47
  grammar growth (`%+d`, `% d`, `%#x`, `%#o`, precision `%.5d`/`%.0d`,
  `h`/`hh`/`ll` lengths, runtime `%.Ns`) with hand-rolled exact
  evaluator arms where Python %-formatting diverges from glibc (`%#o`,
  `%#x` of 0, `%.0d` of 0). `%e`/`%g` ARE generated, but only over a
  curated double pool (2.5, -0.75, 144.0, 0.03125) that was verified
  byte-identical between Python and glibc during v5 bring-up; broader
  float formatting (arbitrary values, `%f`/`%E`/`%G` width games) stays
  deferred to the test/EndToEnd/printf-formats.c parity pins. The
  0-flag+precision combo (e.g. `%010.5d`) is deliberately not generated:
  Python pads it differently than glibc. Three templates also sprinkle dead, unreferenced VLA
  declarations with side-effect-free sizes: the importer must elide
  them, and any effect on output is a bug the differ catches.

  Scratch-file hermeticity: the file_io template's filenames embed the
  seed and instance uid, and differ.py runs BOTH binaries with the
  per-seed workdir as their cwd, so parallel jobs can never collide and
  nothing is written outside the harness scratch tree. The never-created
  name for the fopen NULL branch is likewise per-seed. file_io does not
  need special cross-composer handling: templates are sampled without
  replacement, so a program can never contain two file_io instances —
  one scratch file per program by construction. (bitfields and file_io
  are not pointer-provenance features, so neither joins the
  cross-composer's forced-pair set; they still mix freely with every
  other template through normal sampling.) A configurable fraction of seeds (`--cross-fraction`, default
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

## Range-refinement checking mode

`--range-check` adds `--check-range-refinement` to every emitrust-cc
invocation, turning on the differential integer-range checker
(`emitrust-range-refinement-check`) inside the compile. A compile
failure whose stderr contains `range refinement violation` becomes the
hard-fail class `RANGE_VIOLATION` — reported like MISCOMPILE with
artifacts saved, and failing the run under `--fail-on-miscompile`;
every generated program is correct by construction, so any
RANGE_VIOLATION is a checker false positive (a checker bug, never a
generator or program bug). All other compile failures still classify
UNSUPPORTED. The mode does not touch the generator: GENERATOR_VERSION
and the seed→program mapping are byte-identical with and without it.

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

`HARNESS_BUG` means the native leg failed to compile or run;
`GENERATOR_ORACLE_BUG` means the native leg ran but disagreed with the
generator's own expected output. Both are generator defects (UB,
evaluator drift, or unsupported-by-clang output) and must be fixed in
genprog.py before their seed range is trusted — never paper over an
oracle disagreement by exempting a template.
