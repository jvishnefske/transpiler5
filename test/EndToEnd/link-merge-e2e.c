// REQUIRES: cargo
// FR-58 slice 1, link-step whole-program aggregation, end to end over
// FR-56+57+58: each TU is compiled THROUGH THE SHIM (`emitrust-clang -c`),
// which side-emits a per-TU emitrust shard as MLIR bytecode -- embedded in
// the object's `.emitrust` section and mirrored in a sidecar -- and
// `emitrust-cc --link` then extracts, merges, and materializes the crate
// with NO re-parse of any `.c`. The merge must resolve the cross-TU
// obligations this program is built from: a call into the other TU (add,
// apply), an extern global the other TU defines (shared_counter), a
// shared-header struct/enum carried identically by both shards (dedup by
// symbol, first occurrence wins), and a file-static `scale` defined in BOTH
// TUs (per-shard `tu0_` tags alpha-renamed to link-line ordinals). The
// linked crate and the clang-built native binary must produce
// byte-identical stdout, and -- the FR-58 acceptance oracle -- the linked
// crate root must be BYTE-IDENTICAL to the joint single-invocation
// `emitrust-cc --emit=rust` of the same two sources. Since FR-62 F1a-3
// the DEFAULTED --actor-lift lifts under --link (every shim artifact
// carries FR-57d graph metadata), so the merge-equivalence oracle
// compares both sides in the same LIFTED form with no flag on either
// side; both shards are solo, which is exactly where the joint-vs-link
// identity pin is sound.
//
// Per-TU shards through the shim:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-lib.c -o %t.lib.o
//
// Link the objects into a crate and build it:
// RUN: emitrust-cc --link %t.main.o %t.lib.o -o %t.crate --crate-name link_merge --build
//
// Differential oracle against the clang-linked native binary:
// RUN: clang -std=c11 %s %S/Inputs/link-merge-lib.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/link_merge > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
//
// Byte identity vs the joint import (the FR-58 acceptance oracle):
// RUN: emitrust-cc --emit=rust %s %S/Inputs/link-merge-lib.c -o %t.joint.rs
// RUN: diff %t.joint.rs %t.crate/src/main.rs
//
// Sidecar inputs: linking the `.emitrust.mlirbc` sidecars directly instead
// of the objects yields the same bytes:
// RUN: emitrust-cc --link %t.main.o.emitrust.mlirbc %t.lib.o.emitrust.mlirbc --emit=rust -o %t.sidecar.rs
// RUN: diff %t.joint.rs %t.sidecar.rs
//
// Sidecar FALLBACK: an object whose `.emitrust` section is missing (here:
// stripped) falls back to the `<obj>.emitrust.mlirbc` sidecar next to it:
// RUN: llvm-objcopy --remove-section %emitrust_section_spec %t.main.o %t.stripped.o
// RUN: cp %t.main.o.emitrust.mlirbc %t.stripped.o.emitrust.mlirbc
// RUN: emitrust-cc --link %t.stripped.o %t.lib.o --emit=rust -o %t.fallback.rs
// RUN: diff %t.joint.rs %t.fallback.rs

#include "Inputs/link-merge.h"

int printf(const char *, ...);

// Internal-linkage helper sharing its spelling with the library's static.
static int scale(int x) { return x + 1; }

int main(void) {
  struct Point p;
  p.x = 3;
  p.y = 4;
  printf("add=%d\n", add(40, 2));
  printf("shared=%d\n", shared_counter);
  printf("local_scale=%d\n", scale(10));
  printf("apply_raw=%d\n", apply(p, MODE_RAW));
  printf("apply_scaled=%d\n", apply(p, MODE_SCALED));
  return 0;
}
