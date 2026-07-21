// REQUIRES: cargo
// W3.4 G1 EndToEnd differential: a data-pointer fn-ptr RESULT type erased in a
// multi-TU project. The externally visible fn-ptr global `p` returns
// `struct S *`; the only address-taken candidate returning that type (`go`) is
// address-taken in THIS TU alone, so the whole-program candidate-completeness
// fact lets classifyFnPtrPointerResult erase the result to `!emitrust.fn_ptr<()>`
// and route `p()->m` through the single global base `gs`. The companion TU is
// unrelated — its only job is to force the >=2-TU project import path so the
// erasure is a genuine whole-program decision. The crate build and the
// clang-linked native binary must produce byte-identical stdout.
// RUN: emitrust-cc --emit=crate %s %S/Inputs/multi-tu-gate-g1-fnptr-result-erasure-e2e-other.c -o %t.crate --crate-name g1_erase --build
// RUN: clang -std=c11 %s %S/Inputs/multi-tu-gate-g1-fnptr-result-erasure-e2e-other.c -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/g1_erase > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct S {
  int m;
};
struct S gs = {42};

struct S *go(void) { return &gs; }

// Reassigned (non-const) fn-ptr global: keeps `p` out of the devirtualization
// alias so mapping its type genuinely reaches classifyFnPtrPointerResult.
struct S *(*p)(void) = &go;
void swap(void) { p = &go; }

// Defined in the companion TU.
int unrelated_g1(void);

int main(void) {
  printf("m=%d u=%d\n", p()->m, unrelated_g1());
  return 0;
}
