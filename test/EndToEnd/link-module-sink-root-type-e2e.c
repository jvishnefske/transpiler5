// REQUIRES: cargo
// FR-173 D2: a per-TU module emitted for a SUNK record must import the
// crate root.
//
// FR-159 sinks a shape-conflicting, translation-unit-local record into a
// per-TU module -- named after the shard's source file since phase 2, so
// `b.c` gives `mod b { ... }`. The sunk struct_def keeps its FIELD TYPES, and those
// may name records that stayed at the crate ROOT -- here `struct P`, which
// exists in only one shard and so never conflicts and never sinks. Inside
// that module the bare name `P` resolves in the MODULE's namespace, and
// rustc's answer is `error[E0425]: cannot find type 'P' in this scope`: the
// whole crate is lost, with a rustc error rather than a located one, which
// is precisely the silent-degradation direction FR-159's own guard exists
// to prevent. Measured at HEAD on this very program, `emitrust-cc --link
// ... --emit=rust` emitted the sunk module with no import at all and the
// crate did not build.
//
// The fix is `use super::*;` at the head of the module. It is emitted only
// when the module actually reaches a root name -- an unused `use super::*;`
// is a rustc `unused_imports` warning, and every module the emitter
// produced before FR-173 (whose items name nothing at the root) keeps its
// exact bytes; test/Driver/link-merge-module-sink.c and
// test/EndToEnd/link-module-sink-e2e.c pin that no-import case.
//
// `cargo build` succeeding is NOT the oracle here: the change decides which
// Rust TYPE each field has, so the only evidence that counts is the emitted
// crate's stdout against the clang-linked native's. Every value derives
// from argc so neither compiler can constant-fold the answers.
//
// RUN: split-file %s %t
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/a.c -o %t/a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/b.c -o %t/b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/main.c -o %t/main.o
// RUN: emitrust-cc --link %t/a.o %t/b.o %t/main.o -o %t.crate --crate-name sink_root_type --build
//
// The sink happened AND the module carries the import, at the module's own
// indentation, ahead of every item:
// RUN: FileCheck %s --check-prefix=SUNK --strict-whitespace < %t.crate/src/main.rs
//      SUNK:mod b {
// SUNK-NEXT:    use super::*;
// SUNK-NEXT:    #[derive(Clone, Copy, Default)]
// SUNK-NEXT:    pub(crate) struct S {
// SUNK-NEXT:        pub(crate) p: P,
//
// The oracle:
// RUN: clang -std=c11 %t/a.c %t/b.c %t/main.c -o %t.native
// RUN: %t.native a b c > %t.native.out
// RUN: %t.crate/target/release/sink_root_type a b c > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// ... and again at a different argc, so a single accidental agreement at
// one seed cannot pass for equality:
// RUN: %t.native > %t.native1.out
// RUN: %t.crate/target/release/sink_root_type > %t.rust1.out
// RUN: diff %t.native1.out %t.rust1.out

//--- a.c
struct S {
  double d;
  long e;
};
static struct S ga;
int fa(int seed) {
  ga.d = seed * 2;
  ga.e = seed + 5;
  return (int)ga.d + (int)ga.e;
}

//--- b.c
struct P {
  int x;
  int y;
};
struct S {
  struct P p;
  int q;
};
static struct S gb;
int fb(int seed) {
  gb.p.x = seed;
  gb.p.y = seed + 1;
  gb.q = seed * 3;
  return gb.p.x * 100 + gb.p.y * 10 + gb.q;
}

//--- main.c
int printf(const char *, ...);
int fa(int);
int fb(int);
int main(int argc, char **argv) {
  printf("fa=%d fb=%d\n", fa(argc), fb(argc));
  printf("mix=%d\n", fa(argc + 3) - fb(argc + 1));
  return 0;
}
