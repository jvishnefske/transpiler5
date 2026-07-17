// REQUIRES: cargo
// CTS-R4: differential end-to-end test for block-scope struct declarations
// shadowing an outer tag. One function uses the file-scope `struct T` and a
// same-named, different-shaped block-scope `struct T` side by side; a
// second function shadows with an identical shape, which is still a
// distinct C type. Everything is reported via printf so diff covers the
// observable behavior of both scopes' member accesses.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/structs_shadow > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct T {
  int x;
  int y;
};

int both_scopes(void) {
  struct T outer;
  outer.x = 10;
  outer.y = 20;
  {
    struct T { int z; } inner;
    inner.z = outer.x + 3;
    printf("inner z %d\n", inner.z);
    outer.y = outer.y + inner.z;
  }
  printf("outer %d %d\n", outer.x, outer.y);
  return outer.x + outer.y;
}

int same_shape(void) {
  struct T { int x; int y; } local;
  local.x = 7;
  local.y = 9;
  printf("local %d %d\n", local.x, local.y);
  return local.x * local.y;
}

int main(void) {
  printf("both %d\n", both_scopes());
  printf("same %d\n", same_shape());
  return 0;
}
