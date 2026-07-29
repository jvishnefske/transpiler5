// REQUIRES: cargo
// RUN: %python %S/../run_realworld.py --emitrust-cc emitrust-cc --clang clang++ --corpus-kind cpp --manifest-format outcomes --corpus %S/Inputs --manifest %S/expected-outcomes.txt --known-miscompiles %S/known-miscompiles.txt --workdir %t
//
// FR-46 RealWorld C++ corpus gate. This file has a .cpp suffix only so lit
// discovers it (test/lit.cfg.py sets config.suffixes = [".mlir", ".c",
// ".cpp"]); it contains no C++ code. The corpus projects live under Inputs/
// (excluded from lit discovery by config.excludes = ["Inputs"]) and are driven
// by ../run_realworld.py in its --corpus-kind cpp mode, which reads each
// project's checked-in compile_commands.json (relative paths, made absolute at
// run time), transpiles the whole project through emitrust-cc --emit=crate
// --build, differentially validates any project that transpiles against a
// clang++ -std=c++17 native build, and enforces the expected-outcomes.txt
// ratchet.
//
// Purpose (design.md "C++ input (subset)"): this corpus exists to GENERATE
// DEMAND for the C++ input subset, not to be a conformance target. Most
// projects are expected to REJECT with a located diagnostic, tagged by blocker
// category in the runner's ranked tabulation; that ranking is what sequences
// the next C++ input waves. A REJECTED line in the manifest is the honest
// baseline, not a failure. The ratchet is still two-way: a project that stops
// transpiling fails as a regression, and a project that starts transpiling
// fails until it is ratcheted forward with:
//
// FR-51 adds a third outcome, LIB_BUILT, for a project with no `main`:
// emitrust-cc emits a LIBRARY crate and it compiles. `ringbuf-lib` is the
// corpus's one such project and exists to cover that shape, which every other
// project here misses because every other project has an entry point.
// LIB_BUILT is deliberately NOT TRANSPILED -- a library cannot be run, so it
// is never diffed against a native build, and the outcome claims only that
// the project translates and type-checks. It ranks between REJECTED and
// TRANSPILED, and losing it is a regression exactly as losing TRANSPILED is.
//   python3 test/RealWorld/run_realworld.py --emitrust-cc build/bin/emitrust-cc \
//     --clang clang++ --corpus-kind cpp --manifest-format outcomes \
//     --corpus test/RealWorld/Cpp/Inputs \
//     --manifest test/RealWorld/Cpp/expected-outcomes.txt \
//     --known-miscompiles test/RealWorld/Cpp/known-miscompiles.txt \
//     --workdir /tmp/realworld-cpp-work --update
//
// Per-ITEM scoring (how MUCH of a rejected project ported) IS part of this
// gate since FR-44: every project is additionally transpiled with
// `emitrust-cc --emit=crate --incremental --build`, which must produce a
// crate that COMPILES even when most of the project is out of subset, and
// whose emitrust-progress.json is ratcheted against the project's own
// expected-items.txt (next to its sources) with the same two-way discipline.
// The same --update command above regenerates those per-item ledgers.
