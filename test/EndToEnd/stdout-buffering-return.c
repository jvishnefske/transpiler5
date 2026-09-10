// REQUIRES: cargo
// FR-228 acceptance clause 3, other half: a `return` from `main` is the OTHER
// normal termination, and it is flushed by the ENTRY WRAPPER rather than by
// anything the imported C emits.
//
// `fn main() { std::process::exit(c_main()); }` is the shape that made this
// subtle: `std::process::exit` runs no destructors, so the wrapper has to
// flush explicitly between `c_main` returning and the process exiting. This
// pins that it does, for the argc-bearing wrapper flavour, and that the
// status `main` returned still reaches the OS.
//
// The trailing partial line is again the load-bearing part: it is the one
// thing a line-buffered writer would drop and a never-flushed buffer would
// drop, so it is written last and checked last.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name stdout_buffering_return_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: sh -c '%t.native > %t.native.out; echo rc=$? > %t.native.rc'
// RUN: sh -c '%t.crate/target/release/stdout_buffering_return_e2e > %t.rust.out; echo rc=$? > %t.rust.rc'
// RUN: diff %t.native.out %t.rust.out
// RUN: diff %t.native.rc %t.rust.rc
// RUN: FileCheck %s --check-prefix=RC < %t.native.rc
// RUN: FileCheck %s --check-prefix=TAIL < %t.rust.out

// RC: rc=5
// TAIL: no newline here 6{{$}}

#include <stdio.h>

int main(int argc, char **argv) {
  int seed = argc;
  int i;
  for (i = 0; i < 300 * seed; i++)
    printf("line %d %d\n", i, i * 3 + seed);
  printf("no newline here %d", seed * 6);
  return seed * 5;
}
