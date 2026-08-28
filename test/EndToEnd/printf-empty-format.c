// REQUIRES: cargo
// FR-131: the byte-diff oracle for the EMPTY printf format string. Before
// the fix `printf("")` emitted `print!()`, which is not valid Rust -- rustc
// rejects it ("requires at least a format string argument") and the whole
// crate fails to build, so this test could not even reach its diff. That is
// the SAFE failure direction (a hard compile error, never a miscompile) but
// still an exit-0 unbuildable crate. `print!("")` writes zero bytes, which
// is exactly what C's `printf("")` writes, so the emitted stdout must be
// byte-identical to the clang-built native's.
//
// The empty calls are INTERLEAVED with real output -- before, between and
// after partial lines, inside a loop, and under an argc-dependent branch --
// so a fix that dropped the call entirely, emitted a stray newline, or
// reordered a segment would move bytes. Every printed value derives from
// argc so constant folding cannot pre-compute the run and hide a
// miscompile; the second RUN pair re-seeds via extra argv words.
// `cargo build` success alone proves nothing here -- the stdout diff
// against the clang-built native is the oracle. Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/printf_empty_format > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/printf_empty_format a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

int main(int argc, char **argv) {
  int seed = argc * 7 + 3;
  int i;

  /* Empty call ahead of the first byte of output. */
  printf("");
  printf("seed=%d\n", seed);

  /* Empty call BETWEEN two halves of one line: a stray newline or a
     dropped segment would split or fuse the line. */
  printf("a=%d", seed);
  printf("");
  printf(" b=%d\n", seed * 2);

  /* Empty call inside a loop, between the digits and the separator. */
  for (i = 0; i < 4; ++i) {
    printf("%d", i + seed);
    printf("");
    printf(":");
  }
  printf("");
  printf("\n");

  /* Empty call under an argc-dependent branch: the taken side prints a
     partial line, the untaken side must print nothing at all. */
  if (argc > 1) {
    printf("");
    printf("many=%d", argc);
    printf("");
    printf("\n");
  } else {
    printf("");
  }

  /* Empty call as the very last statement before the return. */
  printf("tail=%d\n", seed - argc);
  printf("");
  return 0;
}
