## Performance baseline

Every EndToEnd test's *runtime* is trivial (a handful of loop iterations or
printf calls), so the cost that matters is transpile and transpile+build
wall time, not generated-binary runtime. `tools/bench-transpile.sh` measures
both (bash `time`, no hyperfine dependency) over a fixed set of ten
representative EndToEnd programs. The recorded baseline, reproduction
command, and full per-program table live in
[docs/perf-baseline.md](docs/perf-baseline.md); in short, transpile-only is
~0.4s total across all ten programs while transpile+build is ~8.4s total —
`cargo build --release` dominates, consistent with W1.0's finding that the
full 220-test ledger's ~20s wall time is cargo-dominated. This is the
regression reference for the upcoming ImportC.cpp split and future waves.
