// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST

// W2.23: user copy constructors and C++ value semantics. This file pins the
// IMPORT-LEVEL shape and the EMITTED RUST shape of the admitted subset; the
// byte-diff oracles are test/EndToEnd/cpp-copy-ctor.cpp (the counting table),
// test/EndToEnd/cpp-copy-dtor.cpp (copy+dtor interleaving) and corpus entry
// test/Cpp17Suite/Inputs/00801.cpp (the elision sensor).
//
// THE IMAGE IS NOT derive(Clone) + .clone(): the copy constructor imports as
// an ordinary `&mut self` method taking `&T` (the W2.2 constructor-call
// shape), and the measured rule is ONE constructor call per syntactic AST
// copy point -- C++17 guaranteed elision appears as ABSENT copy nodes, so a
// prvalue argument and a prvalue factory return emit ZERO copy calls with no
// analysis at all. Copy-ASSIGNMENT (`m = a;` through the implicit
// `operator=`) is a DIFFERENT AST node and lowers memberwise: C++ runs
// operator=, not the copy ctor, so a .clone() there would print 1 against
// native 0 (the spike's measured miscompile).
//
// Three markers carry the wave:
// 1. `emitrust.has_copy_ctor` on the class's struct_def -- the emitter drops
//    `Copy` from the derive. WITHOUT it the by-value case is a COMPILE-CLEAN
//    miscompile: Rust's bitwise Copy substitutes for the user's constructor
//    and prints 0 copies where C++ has 1 (measured). Same plumbing as
//    W2.17's `emitrust.has_drop`, different trigger; a copy+dtor class
//    carries both.
// 2. The copy ctor joins the constructor overload set under FR-114's suffix
//    machinery: `Tracer_new_rtracer` (r + record snake tag), and the value
//    ctor gains ITS suffix (`Tracer_new_i`) -- a rename that can only fire
//    in newly-admitted programs, since any class with a user copy ctor was
//    class-level fatal before this wave.
// 3. A by-value argument / return copy is the same call shape into an
//    anonymous temp place, then moved in (the W2.8 std::pair precedent) --
//    a moved-out temp never drops, which is what makes the copy+dtor
//    interleaving correct by construction.

extern "C" int printf(const char *, ...);

// CHECK-DAG: emitrust.struct_def @Tracer ["value"] [i32] {emitrust.has_copy_ctor}
struct Tracer {
  int value;
  Tracer(int v) : value(v) {}
  Tracer(const Tracer &other) : value(other.value) { printf("copy\n"); }
};

// A copy+dtor class carries BOTH markers (attr-dict prints alphabetically).
// CHECK-DAG: emitrust.struct_def @Loud ["id"] [i32] {emitrust.has_copy_ctor, emitrust.has_drop}
struct Loud {
  int id;
  Loud(int i) : id(i) { printf("ctor %d\n", id); }
  Loud(const Loud &o) : id(o.id) { printf("copy %d\n", id); }
  ~Loud() { printf("dtor %d\n", id); }
};

// A POD keeps its trivial whole-value copy AND -- new this wave -- the
// place-init spelling `Plain b = a;` takes the same whole-struct load/store
// the rvalue path always unwrapped.
struct Plain {
  int x;
  int y;
};

// FR-114 names: the two constructors are a genuine overload set.
// CHECK-DAG: func.func @Tracer_new_i(%arg0: !emitrust.mut_ref<!emitrust.struct<"Tracer">>, %arg1: i32) attributes {emitrust.method_of = "Tracer"
// CHECK-DAG: func.func @Tracer_new_rtracer(%arg0: !emitrust.mut_ref<!emitrust.struct<"Tracer">>, %arg1: !emitrust.ref<!emitrust.struct<"Tracer">>) attributes {emitrust.method_of = "Tracer"
// CHECK-DAG: func.func @Loud_new_rloud(%arg0: !emitrust.mut_ref<!emitrust.struct<"Loud">>, %arg1: !emitrust.ref<!emitrust.struct<"Loud">>) attributes {emitrust.method_of = "Loud"
// CHECK-DAG: func.func @Loud_dtor(%arg0: !emitrust.mut_ref<!emitrust.struct<"Loud">>) attributes {emitrust.drop_impl, emitrust.method_of = "Loud"}

int take(Tracer t) { return t.value; }

// A prvalue factory return: `return Tracer(x);` is a ConstructorConversion
// cast around the VALUE construction -- C++17 guaranteed elision, so there
// is NO copy node and NO copy call, just the value ctor into a temp place
// that is moved out (this is 00801's exact shape).
// CHECK-LABEL: func.func @factory
// CHECK: call @Tracer_new_i
// CHECK-NOT: call @Tracer_new_rtracer
Tracer factory(int x) { return Tracer(x); }

// Returning a BY-VALUE PARAMETER is 1 copy at the return (plus 1 at each
// call site) -- a parameter is never an NRVO candidate, so this is fixed by
// the standard, not implementation-defined.
// CHECK-LABEL: func.func @through
// CHECK: emitrust.addr_of
// CHECK: call @Tracer_new_rtracer
Tracer through(Tracer t) { return t; }

// A TWO-return-object function is admissible (1 copy): with two candidate
// locals clang sets NO NRVO flag on either return, so both returns copy --
// contra a naive reading of W2.17's NRVO note, which covers only the
// single-named-local return (see copy-ctor-invalid.cpp).
// CHECK-LABEL: func.func @two_ret
// CHECK: call @Tracer_new_rtracer
// CHECK: call @Tracer_new_rtracer
Tracer two_ret(int pick, int x) {
  Tracer p(x);
  Tracer q(x + 1);
  if (pick)
    return p;
  return q;
}

// The copy+dtor two-return: the same shape; the temps are moved out and
// never drop, while p/q drop in reverse declaration order exactly as C++
// destroys them after the return copy.
// CHECK-LABEL: func.func @loud_two
// CHECK: call @Loud_new_rloud
Loud loud_two(int pick, int x) {
  Loud p(x);
  Loud q(x + 1);
  if (pick)
    return p;
  return q;
}

// CHECK-LABEL: func.func @c_main
int main() {
  Tracer a(1);
  // Place-init copy `Tracer b = a;`: &mut on the new place, & on the
  // source -- the FR-48 borrow mirror in the CONSTRUCTOR-call argument
  // loop, which is what lets the `const Tracer&` parameter bind a bare
  // lvalue at all.
  // CHECK: emitrust.addr_of
  // CHECK: call @Tracer_new_rtracer
  Tracer b = a;
  // By-value argument: temp place + copy call + move in (1 copy).
  // CHECK: call @Tracer_new_rtracer
  int r1 = take(a);
  // Prvalue argument: NO copy node in the AST, NO copy call emitted.
  // CHECK: call @Tracer_new_i
  int r2 = take(Tracer(5));
  Tracer f = factory(6);
  Tracer t2 = two_ret(1, 7);
  // `through(a)` copies once at the call (the by-value argument); the copy
  // at ITS return was already pinned inside @through above.
  // CHECK: call @Tracer_new_rtracer
  Tracer th = through(a);
  Tracer m(9);
  // Whole-object copy-ASSIGNMENT through the implicit operator=: memberwise
  // field assign, ZERO constructor calls past this point.
  // CHECK-NOT: call @Tracer_new_rtracer
  m = a;
  Plain pa;
  pa.x = 1;
  pa.y = 2;
  Plain pb = pa;
  pb.y = 3;
  Loud la(1);
  Loud lb = la;
  Loud lt = loud_two(1, 2);
  return b.value + r1 + r2 + f.value + t2.value + th.value + m.value + pb.x +
         pb.y + lb.id + lt.id;
}

// The emitted Rust: `Copy` is gone from the derive for BOTH marked classes
// (`Clone`/`Default` stay), while the untouched POD keeps all three -- the
// suppression is per-class, not global.
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct Tracer {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct Loud {
// RUST: #[derive(Clone, Copy, Default)]
// RUST-NEXT: struct Plain {
// The copy ctor is an ordinary &mut self method taking &T, named under
// FR-114's r-code (NOT the spike's predicted `_rs` -- the code is the
// record's snake tag name).
// RUST: fn new_i(&mut self, v: i32) {
// RUST: fn new_rtracer(&mut self, other: &Tracer) {
// RUST: fn new_rloud(&mut self, o: &Loud) {
// RUST: impl Drop for Loud {
