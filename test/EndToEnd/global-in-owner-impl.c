// REQUIRES: cargo
// Regression for the pre-existing `'TOTAL' does not reference a valid
// emitrust.global` bug (recorded off-slice by FR-62 slice 5b, in
// design.md): a function taking a pointer parameter that walks a
// caller-local array while also updating a mutable file-scope global. The
// FR-30 owner promotion moves `add_from` into the `Owner_main_values`
// impl, and `emitrust.impl` carries the MLIR SymbolTable trait, so the
// nested global_load/global_store symbol lookups resolved in the impl's
// table and never saw the module-level `emitrust.global @total` —
// verification failed identically with or without --actor-lift. The fix
// resolves global accessors in the enclosing MODULE's table (the
// DataEnumDefOp::lookupFrom precedent); this program must now compile and
// its stdout byte-diff clean against the clang-built native, THE oracle.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/global_in_owner_impl > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int total;               /* mutable file-scope global updated by add_from */
int calls;               /* second global: the store path is hit twice */

void add_from(const int *src, int n) {
  calls = calls + 1;
  for (int i = 0; i < n; i++)
    total += src[i];
}

int main(void) {
  int values[4];         /* caller-local array: owner promotion trigger */
  for (int i = 0; i < 4; i++)
    values[i] = (i + 1) * 3;
  add_from(values, 4);
  add_from(values, 2);
  printf("total=%d calls=%d\n", total, calls);
  return 0;
}
