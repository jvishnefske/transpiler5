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
//   python3 test/RealWorld/run_realworld.py --emitrust-cc build/bin/emitrust-cc \
//     --clang clang++ --corpus-kind cpp --manifest-format outcomes \
//     --corpus test/RealWorld/Cpp/Inputs \
//     --manifest test/RealWorld/Cpp/expected-outcomes.txt \
//     --known-miscompiles test/RealWorld/Cpp/known-miscompiles.txt \
//     --workdir /tmp/realworld-cpp-work --update
//
// Per-ITEM scoring (how MUCH of a rejected project ported) is NOT part of this
// gate; it arrives with FR-44's --incremental mode, which plugs into the
// clearly marked "FR-44 SEAM" block in run_realworld.py.
