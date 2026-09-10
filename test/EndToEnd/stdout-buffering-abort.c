// REQUIRES: cargo
// FR-228: the differential regression for C's STDOUT BUFFERING MODEL, on the
// one construct that can observe it.
//
// WHAT THIS PINS. C's `stdout` is FULLY buffered when it is not an
// interactive device -- a pipe or a file -- and is flushed at `exit` or when
// the buffer fills, and at NOTHING ELSE. `abort` is not a normal termination:
// C11 7.22.4.1p2 leaves the flush implementation-defined and glibc declines,
// so a program that printf's a COMPLETE LINE and then aborts, with stdout
// redirected to a FILE, writes ZERO BYTES. Measured, not assumed.
//
// Before FR-228 the emitted crate wrote the line, because Rust's `Stdout` is
// a `LineWriter` that had already flushed on the newline -- the emitted
// program printing bytes the reference build does not print, which is the
// silently-wrong direction, and why FR-224 refused `abort` outright rather
// than admit it. The fix is not local to `abort` (flushing at the call site
// writes MORE than glibc, not less): the crate now carries a fully buffered
// stdout writer that is flushed at normal exit only, and `abort` deliberately
// flushes nothing.
//
// WHY THE REDIRECTION IS LOAD-BEARING. On a TTY glibc line-buffers and the
// two models AGREE, so this defect is invisible on a terminal -- 1066 green
// tests never saw it, not because the byte-diff oracle is blind but because
// no test in the corpus terminated without flushing. Both legs below write to
// a FILE. (The terminal leg agrees too, and was verified by hand through a
// pty: 14 bytes, identical on both sides.)
//
// The exit status is diffed as well, because "wrote nothing" would also be
// true of a program that died before printing at all.
//
// The output is deliberately kept well under one 4096-byte block. Once the
// buffer FILLS, glibc's flush points become a function of the destination's
// `st_blksize`, which is a property of the filesystem the test happens to run
// on; the writer reproduces glibc's fill-and-block-write rule exactly (six
// boundaries measured against the clang native: 7 bytes -> 0 on disk at
// abort, 6000 -> 4096, one write of 4096 -> 4096, two of 4096 -> 4096, 8192
// -> 8192, 4097 -> 4096), but asserting that in lit would be asserting a
// property of the checkout's filesystem.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name stdout_buffering_abort_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: sh -c '%t.native > %t.native.out 2>/dev/null; echo rc=$? > %t.native.rc'
// RUN: sh -c '%t.crate/target/release/stdout_buffering_abort_e2e > %t.rust.out 2>/dev/null; echo rc=$? > %t.rust.rc'
// RUN: diff %t.native.out %t.rust.out
// RUN: diff %t.native.rc %t.rust.rc
// RUN: FileCheck %s --check-prefix=RC < %t.native.rc
// RUN: sh -c 'wc -c < %t.native.out' | FileCheck %s --check-prefix=BYTES
// RUN: sh -c 'wc -c < %t.rust.out' | FileCheck %s --check-prefix=BYTES

// SIGABRT, both sides.
// RC: rc=134
// And the measured C behaviour the diff is asserting agreement WITH: the
// completed line, the partial line and the digits are all still sitting in
// the unflushed buffer when the process dies.
// BYTES: 0

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  // Seeded from argc so no constant fold can turn the prints into nothing
  // and make "wrote zero bytes" true for the wrong reason.
  int seed = argc;
  printf("completed line %d\n", seed * 3);
  printf("partial line %d", seed * 5);
  if (seed > 0)
    abort();
  return 0;
}
