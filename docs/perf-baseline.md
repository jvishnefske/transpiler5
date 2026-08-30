# Performance Baseline: Transpile and Transpile+Build Wall Time

Every EndToEnd differential test finishes in milliseconds at *runtime* (see
`test/EndToEnd/*.c`): the generated binaries only exercise a handful of
loop iterations or printf calls each, so profiling their execution tells us
nothing about the cost of this project. The cost that actually regresses or
improves as the pipeline changes is wall time spent (a) transpiling C to
Rust and (b) transpiling and then `cargo build --release`-ing the result.
This is confirmed by W1.0's finding that the full 220-test check-emitrust
ledger runs in ~20s dominated by cargo, not by MLIR/transpilation work.

This file records that baseline so future changes (in particular the
upcoming `ImportC.cpp` split, and any subsequent wave) have a concrete
regression reference. The measurement tool is
[`tools/bench-transpile.sh`](../tools/bench-transpile.sh), which uses only
the bash builtin `time` (hyperfine is not present in the dev shell) over a
fixed, explicitly-listed set of ten `test/EndToEnd` programs chosen to span
the cost spectrum: trivial control flow, a real `#include <stdio.h>`,
bit-field packing, a `FILE*` round-trip, `va_list` monomorphization (the
heaviest single case in the corpus), byte-region/raw-memory walks, tagged
unions, switch-to-jump-table dispatch, multi-base pointer provenance, and
variadic printf format dispatch.

## How to reproduce

```
nix develop -c bash tools/bench-transpile.sh build/tools/emitrust-cc 5
```

The script writes only to a self-cleaning `mktemp -d` workdir; it never
touches the repository tree. It reports, per program and per mode, the
median wall-clock time across N=5 iterations, plus a total that is the sum
of the per-program medians.

## Baseline (recorded)

- Date: 2026-07-20
- HEAD commit: `60fc3af348fae3981030e855ed6cf8f0613c11a9` (W1.2: regenerate
  README Scope for the completed 220/220 ledger + known-issues)
- Binary: `build/bin/emitrust-cc`
- N = 5 iterations per program per mode; figure shown is the median

| Program | Transpile-only (s) | Transpile+build (s) |
|---|---:|---:|
| loops.c | 0.035 | 0.662 |
| stdio-include.c | 0.042 | 0.667 |
| bitfields-flags.c | 0.037 | 0.730 |
| stdio-file-roundtrip.c | 0.043 | 0.978 |
| varargs-monomorph.c | 0.045 | 0.970 |
| byte-region-walk.c | 0.046 | 0.905 |
| unions.c | 0.035 | 0.762 |
| switch-dispatch.c | 0.042 | 0.774 |
| pointers-multi-base.c | 0.042 | 0.747 |
| printf-formats.c | 0.042 | 1.247 |
| **TOTAL (sum of medians)** | **0.413** | **8.442** |

**Takeaway:** transpile-only is consistently ~35-50ms per program (~0.4s
total across all ten); transpile+build is ~0.7-1.2s per program (~8.4s
total) — roughly a 20x multiplier. `cargo build --release` is the cost,
not the transpiler. This matches W1.0's finding that the full 220-test
ledger's ~20s wall time is dominated by cargo, not by MLIR/transpilation
work. Any future work that touches `ImportC.cpp` or the pass pipeline
should be judged against the transpile-only column; the transpile+build
column will stay cargo-dominated regardless and is not a useful signal for
transpiler-side regressions.
