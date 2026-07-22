// REQUIRES: cargo
// RUN: %python %S/run_realworld.py --emitrust-cc emitrust-cc --clang clang --corpus %S/Inputs --manifest %S/expected-transpile.txt --known-miscompiles %S/known-miscompiles.txt --workdir %t
//
// Track 4 RealWorld corpus gate. This file has a .c suffix only so lit
// discovers it (test/lit.cfg.py sets config.suffixes = [".mlir", ".c", ".cpp"]);
// it contains no C code. The corpus programs live under Inputs/ (excluded from
// lit discovery by config.excludes = ["Inputs"]) and are driven by
// run_realworld.py, which transpiles each through emitrust-cc --emit=crate
// --build, differentially validates the transpiled ones against a clang-native
// build, and enforces the expected-transpile.txt ratchet.
//
// Purpose (design.md "Track 4 RealWorld corpus"): the corpus is a DEMAND
// signal. Most programs are expected to REJECT with a located diagnostic
// (tagged by blocker category in the runner's output); their rejection
// frequencies rank the remaining deep designs (C99-43 ptr-to-ptr, C99-46
// dynamic memory). Programs that already transpile are pinned as regression
// guards (and runtime perf workloads). After a deep-design wave lets a program
// transpile, regenerate the ledger with:
//   python3 test/RealWorld/run_realworld.py --emitrust-cc build/bin/emitrust-cc \
//     --clang clang --corpus test/RealWorld/Inputs \
//     --manifest test/RealWorld/expected-transpile.txt \
//     --known-miscompiles test/RealWorld/known-miscompiles.txt \
//     --workdir /tmp/realworld-work --update
