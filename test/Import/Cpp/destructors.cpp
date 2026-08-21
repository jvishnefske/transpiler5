// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST

// W2.17: a user-declared destructor becomes `impl Drop for T`. This file
// pins the IMPORT-LEVEL shape (raw func.func/struct_def, pre
// convert-func-to-emitrust) and the EMITTED RUST shape of the admitted
// subset; the byte-diff oracle for it is test/EndToEnd/cpp-destructor.cpp
// and corpus entry test/Cpp17Suite/Inputs/01003.cpp.
//
// Three markers carry the wave, all of them DISCARDABLE attributes riding
// `attr-dict` (no new op, and -- verified -- no assembly-format change,
// so a module with no destructor is byte-identical to before):
//
// 1. `emitrust.has_drop` on the class's `struct_def`. It is what makes
//    the emitter drop `Copy` from the derive list (rustc E0184: `Copy`
//    cannot be implemented for a type with a destructor) AND what makes
//    the binding LIVENESS guard fire -- an object with a destructor can
//    never have its initializer elided into `let x: T;`, because an
//    UNINITIALIZED Rust binding is never dropped and the destructor's
//    side effects would silently vanish (measured; the regression pin is
//    test/EndToEnd/cpp-destructor-deadstore.cpp).
// 2. `emitrust.drop_impl` on the imported destructor's `func.func`, next
//    to the ordinary `emitrust.method_of` placement tag (the W2.2
//    `emitrust.static_method` precedent). `convert-func-to-emitrust`
//    consumes it: the function is routed into a SECOND, trait-carrying
//    `emitrust.impl` for the class and RENAMED to the symbol `drop`.
// 3. `trait_name = "Drop"` on that second `emitrust.impl`.
//
// The destructor's MODULE-LEVEL symbol is `<Struct>_dtor` (post-rename
// `r_dtor`), deliberately NOT `<Struct>_drop`: a user method spelled
// `void drop()` already occupies `<Struct>_drop` (measured), so reusing
// it would silently merge two different functions. The IN-IMPL symbol
// must be exactly `drop` or rustc rejects the impl with E0407, and the
// rename is safe because `emitrust.impl` is a SymbolTable (two classes'
// `drop` cannot collide) and NOTHING in the subset ever calls a
// destructor, so there are no call sites to rewrite.

extern "C" int printf(const char *, ...);

// CHECK-DAG: emitrust.struct_def @Tracer ["id"] [i32] {emitrust.has_drop}
struct Tracer {
  int id;
  Tracer(int i) : id(i) { printf("ctor %d\n", id); }
  void bump(int d) { id += d; printf("bump %d\n", id); }
  ~Tracer() { printf("dtor %d\n", id); }
};

// A field-less RAII guard: the classic shape. It still gets a real
// `struct_def` (a field-less class WITH a method is not degraded to the
// `[u8; 1]` placeholder a field-less AND method-less record gets), so the
// guard idiom is representable.
// CHECK-DAG: emitrust.struct_def @Guard [] [] {emitrust.has_drop}
struct Guard {
  Guard() { printf("guard ctor\n"); }
  ~Guard() { printf("guard dtor\n"); }
};

// The raw import carries the PRE-rename spellings (`emitrust-cc` folds
// them to snake_case later); what matters here is that the destructor is
// an ordinary method-of-tagged function additionally marked drop_impl.
// CHECK-DAG: func.func @Tracer_new({{.*}}) attributes {emitrust.method_of = "Tracer"
// CHECK-DAG: func.func @Tracer_bump({{.*}}) attributes {emitrust.method_of = "Tracer"
// CHECK-DAG: func.func @Tracer_dtor(%arg0: !emitrust.mut_ref<!emitrust.struct<"Tracer">>) attributes {emitrust.drop_impl, emitrust.method_of = "Tracer"}
// CHECK-DAG: func.func @Guard_dtor(%arg0: !emitrust.mut_ref<!emitrust.struct<"Guard">>) attributes {emitrust.drop_impl, emitrust.method_of = "Guard"}

// A member function literally spelled `drop`. It takes the module symbol
// `<Struct>_drop`; the destructor takes `<Struct>_dtor` and only becomes
// `drop` INSIDE its trait impl. If the two shared a symbol one of them
// would be silently lost -- which is the whole reason for the `dtor` base
// name. (A member spelled `dtor` IS a collision and is a located rejection;
// see destructors-invalid.cpp.)
// CHECK-DAG: func.func @Manual_drop({{.*}}) attributes {emitrust.method_of = "Manual"
// CHECK-DAG: func.func @Manual_dtor({{.*}}) attributes {emitrust.drop_impl, emitrust.method_of = "Manual"}
struct Manual {
  int id;
  void drop() { printf("manual %d\n", id); }
  ~Manual() { printf("manual dtor %d\n", id); }
};

int main(void) {
  Tracer a(1);
  a.bump(2);
  Guard g;
  Manual m;
  m.id = 3;
  m.drop();
  printf("end %d\n", a.id);
  return 0;
}

// The emitted Rust: `Copy` is gone from the derive (E0184) while
// `Clone`/`Default` stay, the inherent impl keeps the ordinary methods,
// and a SEPARATE `impl Drop for Tracer` holds `fn drop(&mut self)`.
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct Tracer {
// RUST: #[derive(Clone, Default)]
// RUST-NEXT: struct Guard {}
// RUST: impl Tracer {
// RUST: fn tracer_new(&mut self, i: i32) {
// RUST: fn tracer_bump(&mut self, d: i32) {
// RUST: impl Drop for Tracer {
// RUST-NEXT: fn drop(&mut self) {
// RUST-NEXT: println!("dtor {}", self.id);
// RUST: impl Drop for Guard {
// RUST-NEXT: fn drop(&mut self) {
// RUST-NEXT: println!("guard dtor");
// The user `drop` and the destructor coexist as two distinct items: the
// inherent one keeps its mangled spelling, the trait one is bare `drop`.
// RUST: impl Manual {
// RUST-NEXT: fn manual_drop(&mut self) {
// RUST: impl Drop for Manual {
// RUST-NEXT: fn drop(&mut self) {
// RUST-NEXT: println!("manual dtor {}", self.id);

// The destructor never renders under its module symbol, and never as an
// inherent method: the whole point of the rename is that `r_dtor`/
// `tracer_dtor` disappears from the emitted text.
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=NODTORSYM
// NODTORSYM-NOT: fn tracer_dtor
// NODTORSYM-NOT: fn guard_dtor
