// REQUIRES: cargo
// FR-111: the field_reassign_with_default fuse, narrowed onto Drop types.
// A destructor-carrying struct whose fields are ALL assigned right after
// default construction now fuses to a base-free struct literal
// (`let s: S = S { a: .., b: .., };`) -- no `..S::default()` functional-
// update base exists, so no spurious extra S is constructed and dropped.
// A PARTIALLY initialized droppy struct stays un-fused (the kept base was
// W2.17's measured spurious `dtor 0 0` ahead of the real output), and the
// same holds inside an impl-method body, where the struct_def symbol must
// resolve at MODULE level (`emitrust.impl` is its own SymbolTable).
//
// The printing destructors make drop COUNT and drop ORDER observable, so
// the byte-diff proves the fuse changed no behavior: exactly one dtor line
// per object, in C++'s reverse-declaration order. Every value derives from
// argc, so constant folding cannot pre-compute the answers and hide a
// miscompile behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_dtor_field_init_fuse > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct S {
  int a;
  int b;
  ~S() { printf("dtor %d %d\n", a, b); }
};

// The partial-init object: its dtor prints only the assigned field (the
// other stays unread -- indeterminate in native C++, zero in Rust).
struct P {
  int a;
  int b;
  ~P() { printf("pdtor %d\n", a); }
};

struct M {
  int base;
  int make(int x) {
    S u; // all-fields droppy fuse inside an impl-method body
    u.a = x;
    u.b = x + base;
    printf("made %d %d\n", u.a, u.b);
    return u.a + u.b;
  }
};

int main(int argc, char **argv) {
  S s; // all fields assigned: fuses base-free
  s.a = argc;
  s.b = argc + 1;
  printf("val %d %d\n", s.a, s.b);
  P t; // partial init: stays un-fused, still exactly one pdtor
  t.a = argc + 2;
  printf("partial %d\n", t.a);
  M m;
  m.base = argc + 3;
  int r = m.make(argc + 4);
  printf("got %d\n", r);
  return 0;
}
