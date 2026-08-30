// REQUIRES: cargo
// FR-160 under FR-59 `--partition`, end to end: the generated `#[test]`
// module in a WORKSPACE is a real differential oracle, and it can go red.
//
// `cargo build` success cannot see a miscompile, so this file leans on the
// two oracles that can:
//
// 1. The workspace BINARY's stdout, byte-diffed against the clang-built
//    native. The seeds come from `argc`, so constant folding cannot stand in
//    for the transpiled arithmetic the tests also exercise.
//
// 2. `cargo test` on the emitted workspace. The entries are wrapped in the
//    MEMBERS that define them -- one in `liba`, one in `libb`, none in the
//    binary member -- and both go green, matching the native run.
//
// 3. NON-VACUITY, the point of the whole FR: an entry whose C exit code is
//    nonzero produces a test that FAILS. A generated suite that can only
//    pass is exactly the vacuous pass FR-160 exists to forbid, and a
//    compile-clean workspace would look identical either way.
//
// Shim-built shards, one per member directory:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/test-entry-ws/liba/a.c -o %t.a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/test-entry-ws/libb/b.c -o %t.b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// RUN: rm -rf %t.ws
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws --crate-name tews \
// RUN:   --emit=crate --partition --build --test-entry=test_alpha \
// RUN:   --test-entry=test_beta 2>%t.err
// RUN: not grep FR-160 %t.err
//
// Each test landed in the member that defines its subject, and nowhere else.
// RUN: grep 'super::test_alpha()' %t.ws/liba/src/lib.rs
// RUN: grep 'super::test_beta()' %t.ws/libb/src/lib.rs
// RUN: not grep 'cfg(test)' %t.ws/tews/src/main.rs
//
// Oracle 1 -- the workspace binary against the clang native, argc-seeded.
// RUN: clang -std=c11 %S/Inputs/test-entry-ws/liba/a.c %S/Inputs/test-entry-ws/libb/b.c %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.ws/target/release/tews > %t.ws.out
// RUN: diff %t.native.out %t.ws.out
//
// Oracle 2 -- the generated tests actually run, and pass.
// RUN: cargo test --offline --release --manifest-path %t.ws/Cargo.toml 2>&1 \
// RUN:   | FileCheck %s --check-prefix=PASS
// PASS-DAG: test emitrust_tests::test_alpha ... ok
// PASS-DAG: test emitrust_tests::test_beta ... ok
//
// Oracle 3 -- and they can go red. The C entry exits 7, so the test fails.
// RUN: rm -rf %t.wsf
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.wsf --crate-name tews \
// RUN:   --emit=crate --partition --build --test-entry=test_fails 2>%t.errf
// RUN: not grep FR-160 %t.errf
// RUN: not cargo test --offline --release --manifest-path %t.wsf/Cargo.toml \
// RUN:   2>&1 | FileCheck %s --check-prefix=FAILS
// FAILS: test emitrust_tests::test_fails ... FAILED

int a_sum(int n);
int b_mix(int n);
int test_alpha(void);
void test_beta(void);
int test_fails(void);

int printf(const char *, ...);

int main(int argc, char **argv) {
  int seed = argc * 5 + 2;
  printf("a_sum=%d\n", a_sum(seed));
  printf("b_mix=%d\n", b_mix(seed % 6));
  test_beta();
  printf("alpha=%d fails=%d\n", test_alpha(), test_fails());
  return 0;
}
