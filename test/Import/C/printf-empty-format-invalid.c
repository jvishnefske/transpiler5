// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-131, the stderr twin. `emitPrintMacro`'s `toStderr` flag selects
// `eprint!`/`eprintln!`, so the empty-format defect would be symmetric --
// except that C has NO accepted route to the stderr twins: the only fprintf
// form the importer devirtualizes is the stdout swallow, and every real
// FILE* stream (`stderr` included) keeps its located rejection. This pins
// that the EMPTY format changes nothing about that rejection: it is still
// a located diagnostic, never a silent drop and never an `eprint!()` that
// rustc would refuse. (The reachable stderr twins are the C++ `std::cerr`
// chains, pinned in ../Cpp/ostream-empty-format.cpp.)

#include <stdio.h>

int main(void) {
  fprintf(stderr, "");
  return 0;
}
// CHECK: printf-empty-format-invalid.c:[[@LINE-3]]:11: error: unsupported: fprintf to a FILE* stream (only the devirtualized stdout form is supported)
