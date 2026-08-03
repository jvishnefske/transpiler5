// REQUIRES: cargo
// FR-59 cycle condensation: two directories whose TUs call EACH OTHER
// (ping in cyc_a calls pong in cyc_b and vice versa) cannot be two cargo
// crates -- cargo rejects cyclic path dependencies -- so the partitioner
// CONDENSES the strongly-connected component into one crate, warns naming
// the cycle, and the build still succeeds with stdout byte-identical to
// the clang-native binary. Condensing rather than failing is the FR's
// acceptance: a cycle is a property of the C project, not an error in it.
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-cycle/cyc_a/ping.c -o %t.a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-cycle/cyc_b/pong.c -o %t.b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// RUN: rm -rf %t.ws
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws --crate-name link_cycle --emit=crate --partition --build 2>%t.err
// RUN: FileCheck %s --check-prefix=CYCLE < %t.err
// CYCLE: warning: workspace partition: condensing 'cyc_b' into 'cyc_a'
// CYCLE-SAME: cycle
//
// The condensed workspace has ONE library member (cyc_a absorbed cyc_b)
// plus the binary:
// RUN: ls %t.ws/cyc_a/src/lib.rs %t.ws/link_cycle/src/main.rs
// RUN: not ls %t.ws/cyc_b
// RUN: FileCheck %s --check-prefix=ROOT < %t.ws/Cargo.toml
// ROOT: members = ["cyc_a", "link_cycle"]
//
// Behavior unchanged:
// RUN: clang -std=c11 %S/Inputs/link-cycle/cyc_a/ping.c %S/Inputs/link-cycle/cyc_b/pong.c %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.ws/target/release/link_cycle > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int ping(int n);

int printf(const char *, ...);

int main(void) {
  printf("ping=%d\n", ping(5));
  return 0;
}
