// REQUIRES: cargo
// RUN: %python %S/run_c_testsuite.py --emitrust-cc emitrust-cc --suite %S/../../third_party/c-testsuite --manifest %S/expected-pass.txt --known-miscompiles %S/known-miscompiles.txt --workdir %t
//
// External c-testsuite conformance ledger.  This file has a .c suffix only so
// lit discovers it (test/lit.cfg.py sets config.suffixes = [".mlir", ".c"]);
// it contains no C code.  The real work is run_c_testsuite.py, which runs
// every third_party/c-testsuite tests/single-exec/*.c through
// emitrust-cc --emit=crate --build and enforces the expected-pass.txt ledger.
//
// Tool resolution: lit's ToolSubst (registered for "emitrust-cc" in
// test/lit.cfg.py via llvm_config.add_tool_substitutions) rewrites the bare
// token `emitrust-cc` after --emitrust-cc above to the absolute path of the
// built tool, exactly as it does when the token is a command word.  The flag
// name `--emitrust-cc` itself is NOT rewritten because ToolSubst skips
// matches whose preceding character is '-' (the default pre=".-^/\<" in
// lit/llvm/subst.py).  As a fallback for manual invocation, the script also
// resolves a bare name via shutil.which; lit.cfg.py appends the tool
// directory to PATH, so both routes agree.
//
// Outcome model: most suite tests are UNSUPPORTED (outside the transpiler's
// C11 subset) and that is fine; tests in expected-pass.txt must PASS; any
// successfully transpiled test with wrong runtime behavior is a MISCOMPILE
// and fails the run unless explicitly quarantined in known-miscompiles.txt.
// After extending the transpiler, regenerate the ledger with:
//   python3 test/CTestSuite/run_c_testsuite.py --emitrust-cc build/tools/emitrust-cc \
//     --suite third_party/c-testsuite --manifest test/CTestSuite/expected-pass.txt \
//     --workdir /tmp/ctestsuite-work --update
