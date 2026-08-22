// REQUIRES: cargo
// FR-120 item 1: method calls through struct POINTERS -- `p->m()` and
// `(*p).m()` -- byte-diffed against `clang++ -std=c++17`. The pointer is
// fully ERASED: a region-tracked local receiver resolves to the bound
// object's own place and a pointer PARAMETER receiver to a borrow deref,
// so the emitted crate contains no pointer at all and only the byte-diff
// can certify the erasure preserved behavior.
//
// The matrix, each leg here because it was a distinct failure or a
// measured near-miss:
// * local CONST receiver (`p->get()`) and local MUTATING receiver
//   (`p->bump(..)`) -- both used to die at `emitLValue`'s blanket
//   "unsupported use of pointer variable" rejection;
// * `p->bump(p->get())` through a LOCAL and through a PARAMETER: the
//   E0502 shape. W2.21's Box path materializes the receiver `&mut`
//   ahead of the arguments and measured E0502; this path renders each
//   argument as its own `let` with the receiver borrow as an inline
//   autoref under two-phase borrows, so the collision cannot recur
//   structurally -- pinned here anyway, byte-for-byte;
// * the `(*p).m()` spelling (explicit deref, same lowering);
// * a CONST method through a NON-CONST pointer parameter
//   (`via_mut_param` calls `p->get()`): the slice-screen gap -- the NoOp
//   qualification cast stacked over the LValueToRValue read made
//   `collectSliceParamsImpl` classify the parameter SLICE and call
//   sites died with the scalar-as-slice wording;
// * a `const Counter *` parameter receiver (shared borrow deref);
// * a CONST method through a pointer to a GLOBAL object: the read
//   resolves through the staged-global copy, which is sound for reads
//   only -- the MUTATING sibling is a located rejection
//   ("mutating method call through a pointer to a global object",
//   struct-pointer-methods-invalid.cpp), because no writeback flush
//   exists and staging would silently drop the mutation.
//
// Every value derives from argc, so constant folding cannot pre-compute
// the answers and hide a miscompile behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_struct_pointer_methods > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct Counter {
  int n;
  Counter(int v) : n(v) {}
  int get() const { return n; }
  void bump(int d) { n += d; }
};

struct Cell {
  int v;
  int get() const { return v; }
};

Cell gcell;

// Const method through a CONST pointer parameter: shared borrow deref.
static void via_const_param(const Counter *p) { printf("cp=%d\n", p->get()); }

// The two parameter shapes that used to misfire together: a MUTATING
// call through the parameter, with the E0502 self-derived argument, and
// a CONST method through the same non-const parameter (the slice-screen
// gap's exact trigger).
static void via_mut_param(Counter *p) {
  p->bump(p->get());
  printf("mp=%d\n", p->get());
}

int main(int argc, char **) {
  Counter c(argc);
  Counter *p = &c;
  p->bump(p->get());          // local mutating receiver, E0502 shape
  printf("l=%d\n", p->get()); // local const receiver
  (*p).bump(2);               // the (*p).m() spelling
  printf("s=%d\n", (*p).get());
  via_const_param(&c);
  via_mut_param(&c);
  printf("c=%d\n", c.n); // the mutations really landed on `c`
  gcell.v = argc + 40;
  Cell *gp = &gcell;
  printf("g=%d\n", gp->get()); // const method through a global's pointer
  return 0;
}
