// REQUIRES: cargo
// Differential test combining the rest of the StmtExpr pack: a
// fall-off-the-end non-void function (the synthesized `return 0` path is
// dynamically unreachable, so the native binary's behavior stays
// defined), `__builtin_expect` folding in branch conditions, and an
// int-carrier pointer (the 00214 `extend_brk` shape: a void* built only
// from integer casts and null constants, returned through a function,
// null-tested with `!p`/`p`, and passed along to another function).
// Byte-identical stdout and exit codes against the clang-built native
// binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name mr_expect_carrier_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/mr_expect_carrier_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern int printf(const char *, ...);
typedef unsigned long size_t;

size_t brk_end;

static void *extend(size_t size, size_t align) {
  size_t mask = align - 1;
  void *ret = 0;
  if (__builtin_expect(!!(brk_end == 0), 0))
    return ret;
  brk_end = (brk_end + mask) & ~mask;
  ret = (void *)brk_end;
  brk_end += size;
  return ret;
}

static int is_null(void *p) {
  if (p)
    return 0;
  return 1;
}

static int classify(size_t v) {
  // Both paths return; the fall-off edge below them is statically
  // reachable (clang warns) but dynamically dead, so the synthesized
  // `return 0` never executes and the native binary stays defined.
  if (__builtin_expect(v != 0, 1))
    return 1;
  if (v == 0)
    return 2;
}

int main(void) {
  void *r;
  brk_end = 0;
  r = extend(64, 16);
  if (!r)
    printf("first=null\n");
  else
    printf("first=wrong\n");
  printf("first_is_null=%d\n", is_null(r));
  brk_end = 1024;
  r = extend(4096, 16);
  if (!r)
    printf("second=wrong\n");
  else
    printf("second=ok\n");
  printf("second_is_null=%d brk_end=%lu\n", is_null(r), brk_end);
  printf("classify0=%d classify5=%d\n", classify(0), classify(5));
  return 0;
}
