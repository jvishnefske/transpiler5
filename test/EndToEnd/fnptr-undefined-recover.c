// REQUIRES: cargo
// FR-77 recovery gate: the `--incremental` crate must BUILD when a sibling
// TU carries a fn-ptr constant naming a function no TU defines. Before the
// fix this exact two-TU shape emitted a crate whose `Some(default_csprng)`
// had no item behind it -- rustc E0425, no binary, the `--incremental`
// contract (drop per-item, always hand back a crate that builds) violated on
// real corpus code (tinycrypt ecc.c). No byte-diff against a native binary
// is possible here BY CONSTRUCTION -- clang cannot link this program either
// (undefined reference to default_csprng from the initializer) -- so the
// oracle is: cargo build succeeds (--build), the binary runs, and its
// stdout is exactly the surviving items' output. The seed derives from argc
// so the printed values are opaque to constant folding.
// RUN: emitrust-cc --emit=crate --incremental %s \
// RUN:   %S/Inputs/fnptr-undefined-recover-lib.c \
// RUN:   -o %t.crate --crate-name fnptr_undefined_recover --build 2>%t.err
// RUN: %t.crate/target/release/fnptr_undefined_recover > %t.out
// RUN: FileCheck %s --input-file=%t.out

int printf(const char *, ...);

int offset(int x);

int describe(int n) { return n * 2 + 1; }

int main(int argc, char **argv) {
  int seed = argc * 3; /* 3 when run plain, but opaque to the compiler */
  printf("offset=%d\n", offset(seed));
  printf("describe=%d\n", describe(seed));
  return 0;
}

// CHECK: offset=7
// CHECK-NEXT: describe=7
