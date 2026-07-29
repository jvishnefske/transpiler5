// REQUIRES: cargo
// FR-48: C++ lvalue REFERENCE PARAMETERS, end to end. Before FR-48 every
// reference type rejected outright in `mapType` with "unsupported: reference
// types are not yet supported" -- the first thing the `polygon` RealWorld
// project hits, in a header, before any container work is even reached.
//
// A reference is a pointer that is non-null, never reseated, and never
// subject to arithmetic, so it maps onto the pointer model's existing
// scalar-borrow class rather than a parallel path: `const T&` becomes
// `!emitrust.ref<T>` (`&T`), `T&` becomes `!emitrust.mut_ref<T>` (`&mut T`),
// each use inside the callee derefs the borrow afresh, and each argument
// borrows the caller's place with `emitrust.addr_of`. What makes this a
// differential test rather than an IR pin is that the mapping has three
// independent ways to be silently WRONG and still compile: a `T&` write that
// does not reach the caller's object (borrow of a copy), a read of the
// reference that yields the borrow instead of the referent, and an argument
// bound to the wrong place. Every printed value below is data-dependent on a
// mutation observed through a reference, so any of the three shows up as a
// stdout diff.
//
// Exercised here: `T&` and `const T&` over a scalar; `T&` and `const T&` over
// a struct; a `T&` OUT-parameter whose write the caller must observe; two
// distinct `T&` parameters mutated in one call (the multi-borrow shape);
// reading a reference parameter as a value (not as a borrow) in arithmetic;
// passing a reference parameter ONWARD to another reference parameter (a
// borrow reborrowed across a call boundary); a reference bound to a struct
// MEMBER and to an ARRAY ELEMENT rather than a whole variable; a method
// taking `const T&` and a method taking `T&`; and `a.cmp(a)` -- a const
// method taking a `const T&` that names its own receiver, which is SOUND
// (two shared borrows) and must be accepted rather than swept up by the
// aliasing rejection that covers the mutable case.
//
// The native leg is clang++ -std=c++17; printf is declared `extern "C"` so
// the native build links libc's printf rather than mangling a C++-linkage
// lookup for it, matching cpp-basics.cpp and cpp-method-chain.cpp.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_references > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct Point {
  int x;
  int y;
};

struct Pair {
  Point lo;
  Point hi;
};

// `T&` over a scalar: the write must land in the CALLER's object.
void bump(int &n) { n = n + 1; }

// `const T&` over a scalar, read as a VALUE in arithmetic. If a reference
// read yielded the borrow instead of the referent this would not even build.
int twice(const int &n) { return n * 2; }

// Two distinct `T&` parameters mutated in one call: the multi-borrow shape,
// and the one that would break if both borrows collapsed onto one place.
void swap_ints(int &a, int &b) {
  int t = a;
  a = b;
  b = t;
}

// `const T&` over a struct: member reads through a shared borrow.
int manhattan(const Point &p) {
  int ax = p.x < 0 ? -p.x : p.x;
  int ay = p.y < 0 ? -p.y : p.y;
  return ax + ay;
}

// `T&` over a struct: member writes through a mutable borrow.
void shift(Point &p, const Point &delta) {
  p.x = p.x + delta.x;
  p.y = p.y + delta.y;
}

// Passing a reference parameter ONWARD to another reference parameter: the
// callee's borrow is reborrowed across a second call boundary, so a
// mis-modelled reference argument compounds rather than cancels.
void shift_twice(Point &p, const Point &delta) {
  shift(p, delta);
  shift(p, delta);
}

// A `T&` OUT-parameter, the `bounding_box(pts, lo, hi)` shape from the
// `polygon` corpus project: the caller learns the result ONLY through the
// reference.
//
// Written with `if`s rather than `?:`, and field-wise rather than with
// whole-struct assignment, on purpose. Both avoided constructs are separate
// PRE-EXISTING gaps unrelated to references: in C++ a conditional between two
// lvalues is itself an lvalue ("unsupported assignable expression:
// ConditionalOperator"), and `lo = a` on a class type is an implicit
// copy-assignment OPERATOR call, not a plain store ("unsupported callee").
// Spelling both out keeps this test measuring what it claims to.
void span(const Point &a, const Point &b, Point &lo, Point &hi) {
  lo.x = a.x;
  lo.y = a.y;
  hi.x = a.x;
  hi.y = a.y;
  if (b.x < lo.x)
    lo.x = b.x;
  if (b.y < lo.y)
    lo.y = b.y;
  if (b.x > hi.x)
    hi.x = b.x;
  if (b.y > hi.y)
    hi.y = b.y;
}

class Acc {
public:
  Acc() : sum_(0), n_(0) {}

  // A METHOD taking `const T&`: the receiver borrow and the argument borrow
  // are live at the same call.
  void add(const Point &p) {
    sum_ = sum_ + p.x + p.y;
    n_ = n_ + 1;
  }

  // A METHOD taking `T&`, writing the receiver's state out through it.
  void drain(int &out) {
    out = sum_;
    sum_ = 0;
  }

  // A const method taking `const T&`. Called below as BOTH `a.cmp(b)` and
  // `a.cmp(a)`; the latter borrows one object twice, which is sound because
  // both borrows are shared.
  int cmp(const Acc &other) const { return sum_ - other.sum_; }

  int sum() const { return sum_; }
  int n() const { return n_; }

private:
  int sum_;
  int n_;
};

int main(void) {
  // --- scalars
  int v = 41;
  bump(v);
  bump(v);
  printf("v=%d twice=%d\n", v, twice(v));

  int lhs = 7;
  int rhs = 90;
  swap_ints(lhs, rhs);
  printf("swapped=%d,%d\n", lhs, rhs);

  // --- structs
  Point p;
  p.x = 3;
  p.y = -4;
  printf("manh=%d\n", manhattan(p));

  Point d;
  d.x = 2;
  d.y = 5;
  shift(p, d);
  printf("shifted=(%d,%d)\n", p.x, p.y);

  shift_twice(p, d);
  printf("shifted twice=(%d,%d) manh=%d\n", p.x, p.y, manhattan(p));

  // --- out-parameters, and references bound to struct MEMBERS rather than
  // --- to whole variables.
  Point a;
  a.x = -3;
  a.y = 9;
  Point b;
  b.x = 12;
  b.y = 1;
  // `lo`/`hi` are separate variables rather than two members of one `Pair`:
  // the pre-existing same-root aliasing rejection is conservative and treats
  // `span(a, b, box.lo, box.hi)` as two borrows of `box`, even though Rust
  // would accept the two disjoint field borrows.
  Point lo;
  Point hi;
  span(a, b, lo, hi);
  printf("span=(%d,%d)-(%d,%d)\n", lo.x, lo.y, hi.x, hi.y);

  // A reference bound to a struct MEMBER rather than to a whole variable:
  // one borrow, so the same-root check does not apply.
  Pair box;
  box.lo.x = 100;
  box.lo.y = 200;
  box.hi.x = 300;
  box.hi.y = 400;
  bump(box.lo.x);
  shift(box.hi, d);
  printf("box=(%d,%d)-(%d,%d)\n", box.lo.x, box.lo.y, box.hi.x, box.hi.y);

  // A reference bound to an ARRAY ELEMENT.
  int cells[4];
  cells[0] = 10;
  cells[1] = 20;
  cells[2] = 30;
  cells[3] = 40;
  // One borrow per call, for the same conservative-same-root reason as
  // `span` above: `swap_ints(cells[0], cells[3])` would be rejected as two
  // borrows of `cells`.
  bump(cells[2]);
  bump(cells[2]);
  printf("cells=%d,%d,%d,%d twice=%d\n", cells[0], cells[1], cells[2],
         cells[3], twice(cells[1]));

  // --- methods
  Acc acc;
  Point q;
  q.x = 3;
  q.y = 4;
  acc.add(q);
  q.x = 10;
  q.y = 20;
  acc.add(q);
  printf("acc sum=%d n=%d\n", acc.sum(), acc.n());

  int drained = -1;
  acc.drain(drained);
  printf("drained=%d after=%d\n", drained, acc.sum());

  Acc other;
  other.add(q);
  // `acc.cmp(acc)` borrows one object twice, both shared: sound, accepted.
  printf("cmp=%d self=%d\n", acc.cmp(other), acc.cmp(acc));
  return 0;
}
