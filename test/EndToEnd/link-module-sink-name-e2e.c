// REQUIRES: cargo
// FR-159 phase 2, the byte-diff oracle for stem-named sink modules.
//
// Phase 2 renames every sunk per-TU module from the shard's link-line ordinal
// (`mod tu1`) to its source file's stem (`mod d1_same`). That rewrites the
// PATH of a Rust type at every one of its uses -- the struct's own definition,
// the signatures of the statics that take it, the struct literals that build
// it -- so the failure mode it can produce is not a build failure but a crate
// where one TU's arithmetic runs on another TU's field layout. `cargo build`
// exiting 0 cannot see that; only the emitted crate's stdout against the
// clang-linked native's can, which is what this file checks.
//
// The program is the hard case for the disambiguator: THREE translation units
// define `struct Buf` with three DIFFERENT shapes, and two of the three source
// files have the SAME stem (`d1/same.c`, `d2/same.c`). So both sink, both need
// a module, and their stems collide. Each must climb to its own directory
// component and land on a distinct module; a disambiguator that merged them
// would give one TU the other's field layout, and every printed value here is
// a distinct function of argc, so any such swap shows up as a stdout diff.
//
// The FileCheck leg is not redundant with the diff: without it a regression
// back to `mod tu1`/`mod tu2` would still byte-diff clean (ordinals are also
// unique), and the point of the phase is that the names come from the FILES.
//
// Every value derives from argc, so neither rustc nor clang can constant-fold
// the answers and a miscompile cannot hide behind a folded literal.
//
// RUN: split-file %s %t
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/base.c -o %t/base.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/d1/same.c -o %t/d1/same.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/d2/same.c -o %t/d2/same.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/main.c -o %t/main.o
// RUN: emitrust-cc --link %t/base.o %t/d1/same.o %t/d2/same.o %t/main.o -o %t.crate --crate-name module_sink_name --build
//
// Both sinks happened, both are named after their FILES, and no ordinal name
// survives:
// RUN: FileCheck %s --check-prefix=NAMES --strict-whitespace < %t.crate/src/main.rs
//      NAMES:mod d1_same {
// NAMES-NEXT:    #[derive(Clone, Copy, Default)]
// NAMES-NEXT:    pub(crate) struct Buf {
// NAMES-NEXT:        pub(crate) a: i32,
// NAMES-NEXT:        pub(crate) b: i32,
// NAMES-NEXT:    }
// NAMES-NEXT:}
//      NAMES:mod d2_same {
// NAMES-NEXT:    #[derive(Clone, Copy, Default)]
// NAMES-NEXT:    pub(crate) struct Buf {
// NAMES-NEXT:        pub(crate) a: i32,
// NAMES-NEXT:        pub(crate) b: i32,
// NAMES-NEXT:        pub(crate) c: i32,
// NAMES-NEXT:    }
// NAMES-NEXT:}
// RUN: FileCheck %s --check-prefix=NOORDINAL < %t.crate/src/main.rs
// NOORDINAL-NOT: mod tu
//
// The oracle:
// RUN: clang -std=c11 %t/base.c %t/d1/same.c %t/d2/same.c %t/main.c -o %t.native
// RUN: %t.native a b c > %t.native.out
// RUN: %t.crate/target/release/module_sink_name a b c > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// ... and again at a different argc, so a single accidental agreement at one
// seed cannot pass for equality:
// RUN: %t.native > %t.native1.out
// RUN: %t.crate/target/release/module_sink_name > %t.rust1.out
// RUN: diff %t.native1.out %t.rust1.out

//--- base.c
struct Buf {
  int a;
};
static int local_use(struct Buf b) { return b.a; }
int f0(int seed) {
  struct Buf b;
  b.a = seed + 11;
  return local_use(b);
}

//--- d1/same.c
struct Buf {
  int a;
  int b;
};
static int local_use(struct Buf b) { return b.a * 10 + b.b; }
int f1(int seed) {
  struct Buf b;
  b.a = seed + 3;
  b.b = seed + 4;
  return local_use(b);
}

//--- d2/same.c
struct Buf {
  int a;
  int b;
  int c;
};
static int local_use(struct Buf b) { return b.a * 100 + b.b * 10 + b.c; }
int f2(int seed) {
  struct Buf b;
  b.a = seed + 5;
  b.b = seed + 6;
  b.c = seed + 7;
  return local_use(b);
}

//--- main.c
int printf(const char *, ...);
int f0(int);
int f1(int);
int f2(int);
int main(int argc, char **argv) {
  printf("f0=%d f1=%d f2=%d\n", f0(argc), f1(argc), f2(argc));
  printf("mix=%d\n", f0(argc + 1) + f1(argc + 2) - f2(argc + 3));
  return 0;
}
