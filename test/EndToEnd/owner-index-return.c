// REQUIRES: cargo
// Stage 1 of the owner-index-return extension (design.md FR-30 follow-on),
// differential end-to-end test: transpile to a cargo crate, build it, and
// compare its stdout against the natively compiled C program. `bigger`
// takes a pointer into `main`'s local array and RETURNS a pointer into the
// same array (the previous owner-struct promotion only handled pointer
// PARAMETERS; a pointer return was an unconditional rejection). Every
// return site of `bigger` is proven to root in the same owner class as its
// own parameter, so it promotes to a `&mut self` method whose Rust result
// type is a plain i64 element index. Callers bind LOCALS directly from the
// call's result (`int *hi1 = bigger(&arr[0]);`) — the novel piece: the
// local's cursor comes from the call, not from an address form — and then
// loop over several starting indices, dereferencing and printing through
// the bound locals. The final grep proves the owner codegen actually
// fired: the emitted crate contains an `impl Owner_...` block. All cursors
// stay in bounds (`bigger` never reads past the last element); the program
// has no UB. main returns 0 and reports everything via printf.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: grep "impl Owner_main_arr" %t.crate/src/main.rs
// RUN: not grep "unsafe" %t.crate/src/main.rs
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/owner_index_return > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

/* Owner-index-return callee: returns a pointer to whichever of *p and its
   successor holds the larger value. Both return sites root in the same
   owner class as the parameter, so `bigger` becomes
   `fn bigger(&mut self, p: i64) -> i64`. */
int *bigger(int *p) {
  if (p[1] > p[0])
    return p + 1;
  return p;
}

int main(void) {
  int arr[6] = {3, 9, 2, 8, 1, 7};

  /* Caller binds a local directly from the call's result: the local's
     cursor is the call's i64 return value, not an address form. */
  int *hi1 = bigger(&arr[0]);
  printf("hi1=%d\n", *hi1);

  int *hi2 = bigger(&arr[2]);
  printf("hi2=%d\n", *hi2);

  /* Loop over several starting indices, calling the method and
     dereferencing the returned pointer through a freshly bound local each
     time. */
  int i;
  for (i = 0; i < 4; i++) {
    int *cur = bigger(&arr[i]);
    printf("i=%d val=%d\n", i, *cur);
  }

  return 0;
}
