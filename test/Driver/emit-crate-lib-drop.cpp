// W2.17: the export-mode half of the Drop rendering. In a LIBRARY crate
// the emitter prefixes every item with `pub` -- but rustc rejects a
// visibility qualifier on a trait-impl member (measured: "error[E0449]:
// visibility qualifiers are not permitted here"), so `fn drop` inside
// `impl Drop for T` must render with NO prefix while the class's inherent
// methods still take theirs. That asymmetry is invisible in a binary
// crate (nothing carries `pub` there), which is why it gets its own pin
// here rather than riding test/Target/Rust/drop-impl.mlir.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.lib
// RUN: cat %t.lib/src/lib.rs | FileCheck %s

extern "C" int printf(const char *, ...);

struct R {
  int id;
  R(int i) : id(i) { printf("ctor %d\n", id); }
  int get() const { return id; }
  ~R() { printf("dtor %d\n", id); }
};

int use_it(int n) {
  R a(n);
  return a.get();
}

// The struct and its inherent methods are exported... (FR-110: the
// members print their in-impl spellings -- `new`/`get`, not the mangled
// `r_new`/`r_get` module symbols, which now live only in the IR)
// CHECK: pub struct R {
// CHECK: impl R {
// CHECK: pub fn new(&mut self, i: i32) {
// CHECK: pub fn get(&self) -> i32 {
// CHECK-NOT: fn r_new
// CHECK-NOT: fn r_get
// ...and the Drop member is NOT (E0449).
// CHECK: impl Drop for R {
// CHECK-NEXT: fn drop(&mut self) {
// CHECK-NOT: pub fn drop
