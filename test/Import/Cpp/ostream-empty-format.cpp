// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=NOBARE

// FR-131, the stderr twin at the IR level. W2.22's `std::cout`/`std::cerr`
// chains are the only producers of the stderr print macros, and they reach
// `emitPrintMacro` through a `flushSegment` that already skips a segment
// with neither format text nor operands. So an all-empty chain emits
// NOTHING at all (correct: C++ writes zero bytes), and the only empty
// format that ever reaches the macro is the residue of a folded `std::endl`
// -- which must keep spelling the BARE `eprintln!()`/`println!()`, never
// `eprintln!("")` (clippy::println_empty_string) and never the invalid
// `eprint!()`. Pinned here so FR-131's non-`ln` fix cannot leak into the
// stderr path in either direction.

#include <iostream>

// CHECK-LABEL: func.func @empty_chains
void empty_chains(void) {
  // No format text, no operands: no macro call is emitted for either stream.
  std::cerr << "";
  std::cout << "";
  // CHECK-NOT: emitrust.call_opaque
  // CHECK: return
}

// CHECK-LABEL: func.func @endl_residue
void endl_residue(void) {
  // The endl fold leaves an empty format; the BARE `ln` spelling stands.
  // CHECK: emitrust.call_opaque "eprintln!"() {args = []} : () -> ()
  std::cerr << "" << std::endl;
  // CHECK: emitrust.call_opaque "println!"() {args = []} : () -> ()
  std::cout << "" << std::endl;
}

// NOBARE-NOT: emitrust.call_opaque "eprint!"() {args = []}
// NOBARE-NOT: emitrust.call_opaque "print!"() {args = []}
// NOBARE-NOT: emitrust.call_opaque "eprintln!"() {args = [""]}
// NOBARE-NOT: emitrust.call_opaque "println!"() {args = [""]}
