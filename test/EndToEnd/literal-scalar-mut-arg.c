// REQUIRES: cargo
// FR-121: differential end-to-end test for a string literal handed to a
// MUTABLE scalar-reference parameter. A deref-only `const char *`
// parameter maps to `&mut i8` (non-u8 const pointees stay mutable, FR-55
// scope), so before FR-121 the call borrowed the literal's cached CONST
// backing mutably -- `&mut v[0]` against a non-`mut` `let`, rustc E0596,
// an emitted crate that did not compile (the spdlog
// `sink(__FILE__, line, "msg")` assert-fail shape, twice per unit).
// The fix rematerializes a FRESH mutable per-call copy of the backing,
// the same model the mutable-slice branch uses for the sliced spelling
// (writing through a pointer to a string literal is UB, so the copy is
// unobservable). The probe covers the three argument spellings: a direct
// literal, a literal-bound pointer variable, and the same pointer after
// cursor arithmetic -- plus a SHARED read of the same literal through the
// untouched const backing, so a fix that mutated the cached backing in
// place instead of copying would diverge here.
// main returns 0 and reports everything through printf, so lit's
// per-command exit-code checking covers both runs and diff covers the
// observable behavior. The program is deterministic and has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/literal_scalar_mut_arg > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

static int first(const char *s) { return *s; }

int main(void) {
  const char *p = "Wxyz";
  int a = first(p);      /* fresh copy #1, cursor 0 */
  p = p + 2;
  int b = first(p);      /* fresh copy #2, cursor 2 */
  int c = (int)*p;       /* shared read of the SAME literal, const backing */
  int d = first("Az");   /* direct-literal spelling */
  printf("%d %d %d %d\n", a, b, c, d);
  return 0;
}
