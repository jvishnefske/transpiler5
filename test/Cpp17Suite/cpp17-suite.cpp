// REQUIRES: cargo
// RUN: %python %S/run_cpp17_suite.py --emitrust-cc emitrust-cc --suite %S/Inputs --manifest %S/expected-pass.txt --known-miscompiles %S/known-miscompiles.txt --workdir %t
//
// In-repo C++17 feature-corpus conformance ledger.  This file has a .cpp
// suffix only so lit discovers it (test/lit.cfg.py sets config.suffixes =
// [".mlir", ".c", ".cpp"]); it contains no C++ code.  The real work is
// run_cpp17_suite.py, which runs every Inputs/NNNNN.cpp through
// emitrust-cc --emit=crate --build and enforces the expected-pass.txt
// ledger.  The corpus files live under Inputs/ precisely because lit's
// config.excludes skips that directory -- they are corpus entries, not
// individual lit tests.
//
// Tool resolution: lit's ToolSubst (registered for "emitrust-cc" in
// test/lit.cfg.py) rewrites the bare token `emitrust-cc` after
// --emitrust-cc above to the absolute path of the built tool; the flag
// name `--emitrust-cc` itself is NOT rewritten because ToolSubst skips
// matches whose preceding character is '-'.  As a fallback for manual
// invocation, the script also resolves a bare name via shutil.which.
//
// Outcome model: corpus entries at the frontier (008xx sensor, 009xx
// markers, unimplemented feature groups) are UNSUPPORTED and that is the
// legal steady state -- the UNSUPPORTED count is the frontier measurement;
// tests in expected-pass.txt must PASS; any successfully transpiled test
// with wrong runtime behavior is a MISCOMPILE and fails the run unless
// explicitly quarantined in known-miscompiles.txt.
//
// The committed NNNNN.cpp.expected files are the native-clang++ reference
// stdout, generated ONCE at corpus-authoring time and committed; regenerate
// one with:
//   nix develop -c sh -c 'clang++ -std=c++17 test/Cpp17Suite/Inputs/NNNNN.cpp \
//     -o /tmp/nnnnn && /tmp/nnnnn > test/Cpp17Suite/Inputs/NNNNN.cpp.expected'
// After extending the transpiler, ratchet the ledger with:
//   python3 test/Cpp17Suite/run_cpp17_suite.py --emitrust-cc build/tools/emitrust-cc \
//     --suite test/Cpp17Suite/Inputs --manifest test/Cpp17Suite/expected-pass.txt \
//     --workdir /tmp/cpp17suite-work --update
