// RUN: emitrust-import-c %s | FileCheck %s

// FR-116: pins the IMPORT-LEVEL contract that the actor-lift fix depends
// on. A non-const global touched from a C++ METHOD BODY is fully admitted
// by the importer -- the global stays an `emitrust.global` and the method
// keeps a real `emitrust.global_load` / `emitrust.global_store` on it, for
// a read, a write, a constructor initializer, and a destructor body alike.
// This is the input the actor-lift pass must survive: the accesses live
// inside functions that later become `emitrust.impl` members, which is
// exactly the region MLIR's SymbolTable use-walk refuses to enter. Nothing
// here is a plan decision; the plan/pass behavior is pinned in
// test/Driver/actor-lift-cpp-method-global.cpp and
// test/Conversion/ActorLift/impl-method-use.mlir. Pinning it separately
// matters because if the importer ever stopped emitting these accesses
// (e.g. by folding the global away), the pass-level pins would go green for
// the wrong reason.

// CHECK: emitrust.global @g <7 : i32> : i32
int g = 7;
// CHECK: emitrust.global @w <0 : i32> : i32
int w = 0;

struct S {
  int v;
  // The constructor's member-initializer list reads the global.
  // CHECK-LABEL: func.func @S_ctor
  // CHECK: emitrust.global_load @g : i32
  S(int x) : v(x + g) {}
  // A const method reading it.
  // CHECK-LABEL: func.func @S_peek
  // CHECK: emitrust.global_load @g : i32
  int peek() const { return g + v; }
  // A method WRITING it: load, add, store, all inside the method.
  // CHECK-LABEL: func.func @S_stir
  // CHECK: emitrust.global_load @w : i32
  // CHECK: emitrust.global_store %{{.*}}, @w : i32
  void stir() { w = w + v; }
  // A destructor body writing it (W2.17).
  // CHECK-LABEL: func.func @S_dtor
  // CHECK: emitrust.global_store %{{.*}}, @w : i32
  ~S() { w = w + 1; }
};

// CHECK-LABEL: func.func @c_main
int main(int argc, char **) {
  S s(argc);
  s.stir();
  return s.peek();
}
