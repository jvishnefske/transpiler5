// RUN: emitrust-import-c %s | FileCheck %s

// A class with an EXPLICITLY defaulted special member (`C() = default;`)
// standing beside a real user constructor used to SIGSEGV the importer: the
// method walk iterated the defaulted ctor into `importFunction`, which asked
// for its (null) `getBody()` and dereferenced it in the statement walk. The
// defaulted member is now skipped by the two import passes exactly like the
// implicit member it stands in for (a defaulted default ctor is realized
// structurally as `derive(Default)`, never as an imported body); only the
// real `C(int)` constructor is imported, and the class imports clean. The
// companion `cpp-defaulted-ctor-invalid.cpp` pins that a *defaulted copy*
// ctor is still rejected rather than silently skipped.

struct C {
  int x;
  C(int v) { x = v; }
  C() = default;
};

int use() {
  C c(3);
  return c.x;
}

// CHECK: emitrust.struct_def @C ["x"] [i32]
// The real constructor imports as an ordinary &mut-self method; the
// defaulted default ctor produces no function at all.
// CHECK: func.func @C_ctor_i(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"C">>, %{{.*}}: i32)
// CHECK: func.func @use_()
// CHECK: call @C_ctor_i(%{{.*}}, %{{.*}}) {emitrust.method_call}
