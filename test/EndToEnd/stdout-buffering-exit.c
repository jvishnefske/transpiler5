// REQUIRES: cargo
// FR-228 acceptance clause 3, and the regression most likely to be traded for
// clause 2: THE NORMAL EXITS MUST STILL FLUSH.
//
// The fix for `abort` is a fully buffered stdout writer, and a buffer is only
// correct if every NORMAL termination empties it. `std::process::exit` DOES
// NOT RUN DESTRUCTORS, so a writer dropped by process exit is never written
// -- and that failure mode is SILENT TRUNCATION, strictly worse than the
// divergence FR-228 set out to fix. This file pins the two shapes that could
// truncate:
//
//   * a TRAILING PARTIAL LINE with no newline. C flushes it at exit; a
//     `LineWriter` never would, and a buffer nobody flushes would lose it.
//     It is the last thing printed here on purpose.
//   * C's own `exit(status)`, lowered to `std::process::exit`, which needs
//     the flush spliced in AHEAD of it at import -- the entry wrapper's
//     flush runs only when `c_main` returns, and this program never does.
//
// Enough rows are printed to cross a 4096-byte stdout block, so the test also
// covers the writer's fill/flush path rather than only its single-buffer
// case; at a normal exit every byte must arrive regardless of where the block
// boundaries fell.
//
// The exit STATUS is diffed too: `exit(seed + 2)` must reach the OS
// unchanged, and a flush inserted in the wrong place (after the exit call, or
// swallowing its argument) would show up here rather than in the bytes.
//
// Both legs redirect stdout to a FILE, which is the configuration in which C
// is fully buffered and therefore the one where a missing flush is
// observable.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name stdout_buffering_exit_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: sh -c '%t.native > %t.native.out; echo rc=$? > %t.native.rc'
// RUN: sh -c '%t.crate/target/release/stdout_buffering_exit_e2e > %t.rust.out; echo rc=$? > %t.rust.rc'
// RUN: diff %t.native.out %t.rust.out
// RUN: diff %t.native.rc %t.rust.rc
// RUN: FileCheck %s --check-prefix=RC < %t.native.rc
// RUN: FileCheck %s --check-prefix=TAIL < %t.rust.out

// RC: rc=3
// The last bytes in the file are the unterminated partial line, so the
// buffer really was flushed by the exit and not merely up to a newline.
// TAIL: trailing partial 1{{$}}

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  // Seeded from argc so constant folding cannot hide a miscompile.
  int seed = argc;
  int i;
  for (i = 0; i < 600 * seed; i++)
    printf("row %d value %d\n", i, i * seed + 7);
  printf("trailing partial %d", seed);
  if (seed > 0)
    exit(seed + 2);
  return 0;
}
