# Corpus sampler — the distinct outcomes of a C corpus, fast

`explore.py` samples C **input files** from a corpus and reports the DISTINCT
translation outcomes emitrust-cc produces — crashes/hangs, blocker-tag
diagnostics, and (for runnable programs) byte-diff miscompiles vs the
clang-native oracle — each with an example file and a one-line reproduction. A
real corpus is highly redundant (CMSIS-DSP's 660 files are a few shapes over
many datatypes; 655 raise the identical diagnostic), so running all of it buys
almost no information past the first few. This finds the distinct outcomes and
stops.

```
# flat corpus, no flags needed:
nix develop -c bash -c 'export EMITRUST_CC=./build/bin/emitrust-cc CLANG=clang; \
  python3 nix/explore/explore.py test/Import/C --seconds 60'

# a project tree that needs include/target flags (appended to every run):
python3 nix/explore/explore.py $CMSIS/Source --recursive \
  --flags "--extra-arg=--target=arm-none-eabi --extra-arg=-I$CMSIS/Include ..."

# escalate runnable programs to the correctness oracle:
python3 nix/explore/explore.py test/EndToEnd --byte-diff
```

Budget: `--seconds S` / `--files N`. `--seed K` reproduces the sample.
`--plateau S` / `--plateau-files N` set the two stop conditions (below).

## Why it is information-dense, not a blind pass

1. **No repeats.** emitrust-cc is deterministic, so each file runs at most once.
2. **Cheapest tier first.** Each sample is a transpile-only probe
   (`--emit=rust -o -`, ~5-15ms) — enough for a crash / diagnostic / clean
   signature. Only a runnable program (`--byte-diff`) escalates to the
   expensive build + byte-diff tier.
3. **Stratified + novelty-guided.** Files are clustered by cheap features
   (directory under the corpus root, size bucket); the sampler spends budget on
   clusters still producing NEW signatures and abandons ones that only
   reproduce known ones — so 70 near-identical files cost a few samples, not 70.
4. **Two plateau stops.** It stops when new outcomes dry up, by EITHER a quiet
   wall-clock window (`--plateau`, matters when per-file cost is high, e.g.
   `--byte-diff` builds) OR a run of samples with no new outcome
   (`--plateau-files`, matters on a big cheap corpus that runs faster than the
   time window). The report says how many files it stopped early.
5. **Parallel + seeded.**

Measured on CMSIS-DSP: **142 of 660 files sampled, 518 stopped early**, same top
outcomes. That is the point of sampling — and its honest cost: an outcome that
occurs in 1 file of 660 can fall past the plateau, so raise `--plateau-files`
when recall of rare outcomes matters more than time.

## The byte-diff oracle is valid only on a clean exit

`MISCOMPILE` fires only when the crate binary exits 0 and its stdout differs
from the clang-native build (both exit 0). A crate that panics (rc 101) where
native exits 0 is a runtime check on C undefined behaviour — the project's
intended safe-failure direction — and is reported as informational `PANIC`, not
a bug (learned from `actor-mode-async-panic.c`, which is deliberately UB
natively).

## Choosing a corpus

- `test/Import/C`, `test/CTestSuite` — low redundancy, many distinct
  constructs; good for a coverage census (most files carry information).
- `nix/corpus` project sources (CMSIS-DSP, lwIP, FreeRTOS) — high redundancy;
  the sampler's savings are largest here. Pass the project's include/target
  flags via `--flags` (see `nix/corpus/default.nix`), and a libc sysroot if the
  files need one, or every sample is a `PARSE_FAIL` on a missing header.
