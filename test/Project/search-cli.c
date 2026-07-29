// FR-43: the driver surface. `--search` is an opt-in that only means
// something where there is an item set to choose, and `--emit=search` is the
// same search with the crate left off, so a project without `main` -- a
// library, which is most of a real codebase -- can still be analyzed.
//
// The point of pinning the rejections rather than letting the flags be
// permissive: `--search` without `--incremental` would silently do nothing
// (a non-incremental crate cannot omit an item, so every candidate but the
// full one is unrepresentable), and a flag that silently does nothing is
// worse than one that refuses.
// RUN: not emitrust-cc --emit=rust --search %s -o /dev/null 2>&1 \
// RUN:   | FileCheck --check-prefix=WRONG-EMIT %s
// WRONG-EMIT: error: --search is only valid with --emit=crate --incremental or --emit=search

// RUN: not emitrust-cc --emit=crate --search %s -o %t.crate 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-INCREMENTAL %s
// NO-INCREMENTAL: error: --search requires --incremental

// `--emit=search` needs no `main` and writes no crate, so it works on a
// translation unit that is a library.
// RUN: emitrust-cc --emit=search %s -o - | FileCheck %s

int library_entry(int value) { return value + 1; }

int library_helper(int value) { return library_entry(value) * 2; }

// CHECK:      search items=2 roots=2 max-nodes=8
// CHECK-NEXT: root 0 admitted=2 excluded=0
// CHECK-NEXT: probe 0 outcome=ok ported=2 stubbed=0 rep-cost=0 dropped=0
// CHECK:      summary probes=1 generated=1 pruned=0 improved=no

// `--search-trace` writes the same text beside a real crate build, so the
// question "why is this item not in my crate?" is answerable from the crate
// the user actually has.
// RUN: emitrust-cc --emit=search %s -o %t.direct
// RUN: emitrust-cc --emit=crate --incremental --search --search-trace=%t.side \
// RUN:   %S/Inputs/search-cli-main.c %s -I %S/Inputs -o %t.crate2
// RUN: FileCheck --check-prefix=SIDE %s < %t.side
// SIDE:      search items=3 roots=3 max-nodes=8
// SIDE:      admitted c_main rep=default
// SIDE-NEXT: admitted library_entry rep=default
// SIDE-NEXT: admitted library_helper rep=default
