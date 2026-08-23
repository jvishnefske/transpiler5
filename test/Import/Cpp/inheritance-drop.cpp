// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=NODTOR
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUSTNODROP

// W2.26: polymorphic RAII, value-only subset -- TRANSITIVE has_drop. This
// file pins the import-level shape of destructor-carrying INHERITANCE
// chains; the byte-diff oracle is test/EndToEnd/cpp-inheritance-drop.cpp
// and corpus entry test/Cpp17Suite/Inputs/01008.cpp.
//
// The invariant pinned here, per shape:
//
// 1. THE MERELY-INHERITING SYNTHESIS: a derived class with no `~D` of its
//    own over a droppy base gets ONLY a transitive `emitrust.has_drop` on
//    its struct_def -- NO `impl Drop`, no `<D>_dtor` func (the NODTOR
//    prefix). Rust's field-drop glue runs `~B` exactly once (measured
//    byte-identical at one, two and three levels). The attr is what makes
//    the emitter suppress `Copy` (the forgotten-case backstop is a LOUD
//    rustc E0204: derive(Copy) beside the base field's Drop) and keep
//    every binding of the class live.
// 2. The transitive predicate recurses the WHOLE single-base chain: a
//    droppy ROOT under a dtor-less middle AND leaf still marks every
//    class in the chain (L below).
// 3. A derived class WITH its own destructor keeps the W2.17 image
//    (has_drop + drop_impl); its drop body runs BEFORE the base field's
//    drop, which is exactly C++'s derived-body-then-base order.
// 4. THE EMPTY DROPPY BASE is MATERIALIZED as a real zero-field
//    struct_def with its own Drop impl, not skipped like a non-droppy
//    empty base -- skipping it was the measured ~Shape-loss trap (the
//    derived object would never run `~Shape` at all).
// 5. A class whose SOLE virtual member is the destructor is admitted as a
//    value (destruction of a value is static; every site where dynamism
//    could be observed -- new, upcast, virtual member call through a
//    pointer -- is already a located rejection). Since W2.19a other
//    virtual methods no longer reject the class either
//    (virtual-methods-values.cpp); the dynamic uses stay pinned in
//    inheritance-drop-invalid.cpp.

extern "C" int printf(const char *, ...);

// CHECK-DAG: emitrust.struct_def @B ["x"] [i32] {emitrust.has_drop}
struct B {
  int x;
  B(int v) : x(v) { printf("ctor B %d\n", x); }
  ~B() { printf("~B %d\n", x); }
};

// Merely inheriting: transitive has_drop, no dtor func of its own.
// CHECK-DAG: emitrust.struct_def @D ["base", "y"] [!emitrust.struct<"B">, i32] {emitrust.has_drop}
struct D : B {
  int y;
  D(int a, int b) : B(a), y(b) {}
};

// Derived WITH its own destructor: has_drop plus a real drop_impl.
// CHECK-DAG: emitrust.struct_def @E ["base", "z"] [!emitrust.struct<"B">, i32] {emitrust.has_drop}
struct E : B {
  int z;
  E(int a, int b) : B(a), z(b) {}
  ~E() { printf("~E %d\n", z); }
};

// Three levels, droppy ROOT only: the recursion walks the whole chain.
// CHECK-DAG: emitrust.struct_def @M ["base", "m"] [!emitrust.struct<"B">, i32] {emitrust.has_drop}
// CHECK-DAG: emitrust.struct_def @L ["base", "l"] [!emitrust.struct<"M">, i32] {emitrust.has_drop}
struct M : B {
  int m;
  M(int a, int b) : B(a), m(b) {}
};
struct L : M {
  int l;
  L(int a, int b, int c) : M(a, b), l(c) {}
};

// The EMPTY droppy base, materialized as a true zero-field struct.
// CHECK-DAG: emitrust.struct_def @Shape [] [] {emitrust.has_drop}
// CHECK-DAG: emitrust.struct_def @Circle ["base", "r"] [!emitrust.struct<"Shape">, i32] {emitrust.has_drop}
struct Shape {
  ~Shape() { printf("~Shape\n"); }
};
struct Circle : Shape {
  int r;
  Circle(int v) : r(v) {}
};

// Sole-virtual-destructor class as a VALUE.
// CHECK-DAG: emitrust.struct_def @V ["id"] [i32] {emitrust.has_drop}
struct V {
  int id;
  V(int i) : id(i) {}
  virtual ~V() { printf("~V %d\n", id); }
};

int use(int n) {
  D d(n, n + 1);
  E e(n, n + 2);
  L l(n, n + 3, n + 4);
  Circle c(n);
  V v(n);
  return d.y + e.z + l.l + c.r + v.id;
}

// The classes that DO declare a destructor carry a drop_impl func...
// CHECK-DAG: func.func @B_dtor(%arg0: !emitrust.mut_ref<!emitrust.struct<"B">>) attributes {emitrust.drop_impl, emitrust.method_of = "B"}
// CHECK-DAG: func.func @E_dtor(%arg0: !emitrust.mut_ref<!emitrust.struct<"E">>) attributes {emitrust.drop_impl, emitrust.method_of = "E"}
// CHECK-DAG: func.func @Shape_dtor(%arg0: !emitrust.mut_ref<!emitrust.struct<"Shape">>) attributes {emitrust.drop_impl, emitrust.method_of = "Shape"}
// CHECK-DAG: func.func @V_dtor(%arg0: !emitrust.mut_ref<!emitrust.struct<"V">>) attributes {emitrust.drop_impl, emitrust.method_of = "V"}

// ...and the merely-inheriting ones carry NONE: their `~B` runs through
// Rust's field-drop glue, exactly once.
// NODTOR-NOT: @D_dtor
// NODTOR-NOT: @M_dtor
// NODTOR-NOT: @L_dtor
// NODTOR-NOT: @Circle_dtor

// The emitted Rust: the transitive attr's one observable is the derive
// list -- Copy is SUPPRESSED on every class in a droppy chain (a plain
// class keeps (Clone, Copy, Default)), and the merely-inheriting classes
// get NO `impl Drop`.
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct B {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct D {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct E {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct M {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct L {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct Shape {}
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct Circle {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct V {
// RUST: impl Drop for B {
// RUST: impl Drop for E {
// RUST: impl Drop for Shape {
// RUST: impl Drop for V {
// RUSTNODROP-NOT: impl Drop for D
// RUSTNODROP-NOT: impl Drop for M
// RUSTNODROP-NOT: impl Drop for L
// RUSTNODROP-NOT: impl Drop for Circle
