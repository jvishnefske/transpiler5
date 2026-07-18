// REQUIRES: cargo
// CTS-S (00089) global-pointer returns, differential end-to-end test:
// transpile to a cargo crate, build it, and compare its stdout against
// the natively compiled C program. Two functions return the address of
// the same mutable global struct; callers write through `go()->member`,
// read back through the arrow chain and directly from the global, and
// the side-effect counter in go() proves the erased-return calls still
// happen. main returns 0 and reports everything via printf, so lit's
// per-command exit-code checking covers both runs and diff covers the
// observable behavior. The program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointers_return_global > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct S { int m; int n; };
struct S gs = { 3, 4 };
int calls;

struct S *go(void) {
  calls++;
  return &gs;
}

struct S *again(void) { return &gs; }

int main(void) {
  printf("start m=%d n=%d\n", gs.m, gs.n);

  /* Write through the erased return; read directly from the global. */
  go()->m = 41;
  printf("wrote m=%d calls=%d\n", gs.m, calls);

  /* Read through the arrow, mutate, write back through the arrow. */
  go()->m = go()->m + 1;
  printf("bumped m=%d calls=%d\n", gs.m, calls);

  /* Chain through the second function, mixing routed and direct reads. */
  again()->n = again()->m + gs.n;
  printf("chained n=%d\n", gs.n);

  /* Direct global write observed through the routed read. */
  gs.m = 7;
  printf("routed m=%d n=%d\n", go()->m, again()->n);

  printf("total calls=%d\n", calls);
  return 0;
}
