// REQUIRES: cargo
// RUN: %python %S/fuzz_differential.py --emitrust-cc emitrust-cc --start 1 --count 16 --jobs 4 --fail-on-miscompile --workdir %t
//
// Differential fuzzing smoke test.  This file has a .c suffix only so lit
// discovers it (test/lit.cfg.py sets config.suffixes = [".mlir", ".c"]); it
// contains no C code.  The real work is fuzz_differential.py, which
// generates 16 deterministic programs (genprog.py, seeds 1..16), runs each
// through both clang and emitrust-cc --emit=crate --build (differ.py), and
// compares exit codes and stdout bytes THREE ways: the generator's own
// expected-output oracle vs the native binary vs the transpiled binary.
// Any MISCOMPILE, GENERATOR_ORACLE_BUG, or HARNESS_BUG fails the test;
// UNSUPPORTED seeds are counted but fine.
//
// Tool resolution: lit's ToolSubst rewrites the bare token `emitrust-cc`
// after --emitrust-cc to the built tool's absolute path, exactly as in
// test/CTestSuite/c-testsuite.c; the script falls back to PATH lookup for
// manual runs.  clang comes from the dev shell's PATH.
//
// The seed range is pinned so the smoke stays deterministic and under a
// minute: same seeds -> byte-identical programs -> stable wall time.  For
// real campaigns see test/Fuzz/README.md.
