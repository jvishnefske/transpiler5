// REQUIRES: cargo
// FR-62 slice 5b, the PANIC-PARITY pin: a deterministic out-of-bounds read
// inside a threaded actor's arm must surface as ONE panic with the
// original provenance and exit code 101 — never a hang, never a double
// panic, never a clean exit. The E3-measured mechanics under test: the
// arm panics on the actor thread, unwinding drops the reply Sender, the
// caller's blocking recv fails immediately, and actor_rt's reap()
// re-raises the actor's own payload in the caller via join +
// resume_unwind (the unconditional clean form). The exit code is the hard
// pin; stderr is best-effort but was deterministic in the spike, so the
// single "panicked" line is pinned too, plus the line-buffered stdout
// prefix that must flush before the panic. The native binary is
// deliberately NOT run: table[7] is C UB, the Rust panic IS the defined
// behavior this test exists to keep.
// RUN: emitrust-cc --actor-mode=threaded --emit=crate %s -o %t.crate --build
// RUN: grep "type TableActorHandle = actor_rt::Handle<TableActorMsg>;" %t.crate/src/main.rs
// RUN: sh -c '%t.crate/target/release/actor_mode_threaded_panic > %t.out 2> %t.err; echo rc=$? > %t.rc'
// RUN: FileCheck %s --check-prefix=RC < %t.rc
// RUN: FileCheck %s --check-prefix=OUT < %t.out
// RUN: sh -c 'grep -c panicked %t.err' | FileCheck %s --check-prefix=ONCE
// RUN: FileCheck %s --check-prefix=ERR < %t.err
//
// RC:   rc=101
// OUT:      t[0]=3
// OUT-NEXT: t[1]=6
// OUT-NEXT: t[2]=9
// OUT-NEXT: t[3]=12
// ONCE: 1
// ERR:  index out of bounds: the len is 4 but the index is 7

int printf(const char *, ...);

int table[4];

void fill(void) {
  for (int i = 0; i < 4; ++i) {
    table[i] = (i + 1) * 3;
  }
}

int get_at(int i) {
  return table[i];
}

int main(void) {
  fill();
  for (int i = 0; i < 4; ++i) {
    printf("t[%d]=%d\n", i, table[i]);
  }
  printf("oob=%d\n", get_at(7));
  return 0;
}
