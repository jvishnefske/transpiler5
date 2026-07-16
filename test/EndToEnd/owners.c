// REQUIRES: cargo
// Phase-4 owner structs, differential end-to-end test: transpile to a
// cargo crate, build it, and compare its stdout against the natively
// compiled C program. Two functions walking one owned local array promote
// to `&mut self` methods of a synthesized `Owner_main_arr` struct: `fill`
// subscripts, `sum` walks its pointer, `total` makes a sibling
// method-to-method call (`(*self).sum(...)`), one call site reads the
// owner in a value argument (`fill(arr, arr[0])` — the read materializes
// before the receiver borrow), and one passes an interior pointer
// (`sum(&arr[2], 4)` — a constant i64 cursor). The final grep proves the
// owner codegen actually fired: the emitted crate contains an
// `impl Owner_...` block. All cursors stay in bounds; the program has no
// UB. main returns 0 and reports everything via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: grep "impl Owner_main_arr" %t.crate/src/main.rs
// RUN: not grep "unsafe" %t.crate/src/main.rs
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/owners > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

/* Subscripting callee: becomes fn fill(&mut self, p: i64, n: i32). */
void fill(int *p, int n) {
  for (int i = 0; i < n; i++) {
    p[i] = i * 3 + 1;
  }
}

/* Walking callee: the i64 index parameter is the walked cursor. */
int sum(int *p, int n) {
  int s = 0;
  while (n > 0) {
    s = s + *p;
    p++;
    n--;
  }
  return s;
}

/* Sibling call: a method calling another method of the same owner. */
int total(int *p, int n) {
  return sum(p, n);
}

int main(void) {
  int arr[8];

  /* Whole-array method calls through the owner. */
  fill(arr, 8);
  printf("sum=%d\n", sum(arr, 8));

  /* Interior-pointer method call: constant cursor 2. */
  printf("tail=%d\n", sum(&arr[2], 4));

  /* A value argument that reads the owner: the element load must be
     materialized before the receiver borrow of the same call. */
  fill(arr, arr[0]);
  printf("refilled=%d\n", sum(arr, 8));

  /* Sibling method-to-method call, from a walked pointer local. */
  int *p = arr;
  p = p + 3;
  printf("fromp=%d\n", total(p, 5));

  return 0;
}
