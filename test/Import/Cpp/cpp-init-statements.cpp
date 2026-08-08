// RUN: emitrust-import-c %s | FileCheck %s
// W2.5: C++17 if/switch init-statements and the condition-declaration form
// import by HOISTING the declaration into the enclosing block, in source
// order, before the condition is evaluated. This pins the desugared shape:
// the hoisted variable's alloca+store precede the condition's cmp/branch
// (or the switch dispatch), and both branch arms read the SAME hoisted
// cell. Scope extension past the `if` is unobservable without destructors
// (outside the subset); a later same-named variable is a distinct decl and
// gets its own cell (the EndToEnd leg, cpp-init-statements.cpp, pins the
// emitted Rust uniquifying such names as r/r_1/r_2).

extern "C" int printf(const char *, ...);

int f(int v) { return v - 1; }

// CHECK-LABEL: func.func @if_init
// CHECK: %[[RCELL:.*]] = memref.alloca() : memref<i32>
// CHECK: %[[RVAL:.*]] = call @f(
// CHECK: memref.store %[[RVAL]], %[[RCELL]][]
// CHECK: %[[R:.*]] = memref.load %[[RCELL]][]
// CHECK: arith.cmpi sgt, %[[R]],
// CHECK: cf.cond_br
// CHECK: memref.load %[[RCELL]][]
// CHECK: memref.load %[[RCELL]][]
int if_init(int v) {
  if (int r = f(v); r > 2) {
    return r;
  } else {
    return -r;
  }
}

// CHECK-LABEL: func.func @cond_decl
// CHECK: %[[CCELL:.*]] = memref.alloca() : memref<i32>
// CHECK: %[[CVAL:.*]] = call @f(
// CHECK: memref.store %[[CVAL]], %[[CCELL]][]
// CHECK: %[[C:.*]] = memref.load %[[CCELL]][]
// CHECK: arith.cmpi ne, %[[C]],
// CHECK: cf.cond_br
int cond_decl(int v) {
  if (int r = f(v)) {
    return r * 2;
  }
  return -1;
}

// CHECK-LABEL: func.func @switch_init
// CHECK: %[[SCELL:.*]] = memref.alloca() : memref<i32>
// CHECK: arith.remsi
// CHECK: memref.store %{{.*}}, %[[SCELL]][]
// CHECK: memref.load %[[SCELL]][]
int switch_init(int v) {
  switch (int r = v % 3; r) {
  case 0:
    return 100 + r;
  default:
    return 300 + r;
  }
}
