// REQUIRES: cargo
// FR-159 phase 1, the byte-diff oracle for the link-time shape-conflict
// SINK. A link that used to be refused now produces a crate, so the only
// evidence that matters is whether the crate's stdout equals the
// clang-linked native's: `cargo build` exiting 0 proves nothing here, since
// the whole change is about which Rust TYPE each value has.
//
// The program deliberately holds BOTH halves of the rule at once:
//   * `struct Buf` is defined in two TUs with DIFFERENT shapes and is used
//     only from internal-linkage statics. The later definition is sunk into
//     `mod tu1`, so the crate carries two distinct `Buf` types on two paths,
//     and each TU's arithmetic must still come out with its own shape's
//     fields. Any confusion between the two is directly visible in stdout.
//   * `struct P` is defined IDENTICALLY in two TUs and crosses a TU boundary
//     by value through the external function `consume`. C99 6.2.7 makes
//     those the SAME type; bucketing records per TU as a matter of course
//     would give rustc `expected tu0::P, found tu1::P`, so this leg pins
//     that records stay at crate root and shape-dedup exactly as before.
// Both must hold in the SAME crate: the sink may not disturb the dedup.
//
// Every value derives from argc, so neither rustc nor clang can constant
// fold the answers and a miscompile cannot hide behind a folded literal.
//
// RUN: split-file %s %t
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/a.c -o %t/a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/b.c -o %t/b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/main.c -o %t/main.o
// RUN: emitrust-cc --link %t/a.o %t/b.o %t/main.o -o %t.crate --crate-name module_sink --build
//
// The sink actually happened -- otherwise this file would be pinning the
// dedup path and nothing else:
// RUN: FileCheck %s --check-prefix=SUNK < %t.crate/src/main.rs
// SUNK: mod tu1 {
// SUNK-NEXT: #[derive(Clone, Copy, Default)]
// SUNK-NEXT: pub(crate) struct Buf {
//
// The oracle:
// RUN: clang -std=c11 %t/a.c %t/b.c %t/main.c -o %t.native
// RUN: %t.native a b c > %t.native.out
// RUN: %t.crate/target/release/module_sink a b c > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// ... and again at a different argc, so a single accidental agreement at
// one seed cannot pass for equality:
// RUN: %t.native > %t.native1.out
// RUN: %t.crate/target/release/module_sink > %t.rust1.out
// RUN: diff %t.native1.out %t.rust1.out

//--- a.c
struct Buf {
  int a;
};
struct P {
  int x;
  int y;
};
static int local_use(struct Buf b) { return b.a; }
int consume(struct P p) { return p.x * 100 + p.y; }
int f1(int seed) {
  struct Buf b;
  b.a = seed + 11;
  return local_use(b);
}

//--- b.c
struct Buf {
  int a;
  int b;
};
struct P {
  int x;
  int y;
};
int consume(struct P p);
static int local_use(struct Buf b) { return b.a * b.b; }
int f2(int seed) {
  struct Buf b;
  struct P p;
  b.a = seed + 3;
  b.b = seed + 4;
  p.x = seed;
  p.y = seed + 7;
  return local_use(b) + consume(p);
}

//--- main.c
int printf(const char *, ...);
int f1(int);
int f2(int);
int main(int argc, char **argv) {
  printf("f1=%d f2=%d\n", f1(argc), f2(argc));
  printf("mix=%d\n", f1(argc + 1) - f2(argc + 2));
  return 0;
}
