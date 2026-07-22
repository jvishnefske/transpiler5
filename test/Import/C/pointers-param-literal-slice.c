// A subscripted (slice) callee called with a `const char *` VARIABLE that
// points into a string literal. Before the literal-backing branch in
// emitBorrowArgument, emitPointerRValue returned a degenerate pointer
// (base == null, literalBacking set) that fell through every guard to a
// null-base error branch which dereferenced `pointer->base->getName()` —
// a SIGSEGV in the importer (RealWorld crc32, W4.1 rank-1 robustness bug).
//
// A subscripted `const char *` parameter is classified as a mutable slice
// (`!emitrust.mut_ref<!emitrust.slice<i8>>`), so a read-only borrow of the
// shared const literal backing would be type-invalid. Instead the call site
// rematerializes a FRESH MUTABLE backing of the literal and passes a whole
// slice from the pointer's cursor — the same model the direct-literal
// argument path (`sum("abc", 3)`) already uses: writing through a pointer
// to a string literal is undefined behavior, so the per-call copy is
// unobservable to any defined program.
//
// RUN: emitrust-import-c %s | FileCheck %s

int printf(const char *, ...);

static unsigned sum(const char *s, int n) {
  unsigned acc = 0;
  for (int i = 0; i < n; i++)
    acc += (unsigned char)s[i];
  return acc;
}

// CHECK-LABEL: func.func @c_main
int main(void) {
  const char *msg = "abc";
  // The fresh mutable backing cloned at the call site (no `const` marker),
  // then a mutable whole-slice of it passed to the slice parameter.
  // CHECK: %[[BACK:.+]] = emitrust.variable <[97 : i8, 98 : i8, 99 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<4xi8>>
  // CHECK: emitrust.slice_of mut %[[BACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
  printf("%u\n", sum(msg, 3));
  return 0;
}
