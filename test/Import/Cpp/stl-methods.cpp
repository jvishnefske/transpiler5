// RUN: emitrust-import-c %s | FileCheck %s
// W2.6: the task-003 STL method expansion — vector front()/back()/
// pop_back() and string push_back(char)/clear(). Pins the chosen Rust
// spellings at the IR level: front/back are SUBSCRIPT PLACES (`v[0]` /
// `v[v.len() - 1]`, index-typed so no `as usize` cast appears), NOT
// Option-returning method chains — C++ front/back on an empty vector is
// UB, so Rust's index panic is a safe refinement; pop_back is a bare
// `pop` whose Option result is deliberately unbound (Vec::pop is not
// #[must_use]; empty-vector pop_back is UB in C++, a no-op None here);
// string push_back reuses operator+='s exact `push(c as char)` emission
// (wrapCharFormat's ASCII policy), and clear mirrors the vector spelling.

extern "C" int printf(const char *, ...);

#include <vector>
#include <string>

// CHECK-LABEL: func.func @vector_ends
// CHECK: %[[C0:.*]] = arith.constant 0 : index
// CHECK: emitrust.subscript %{{.*}}[%[[C0]]] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>, index) -> !emitrust.lvalue<i32>
// CHECK: %[[LEN:.*]] = emitrust.method_call %{{.*}}["len"] () : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>) -> index
// CHECK: %[[C1:.*]] = arith.constant 1 : index
// CHECK: %[[BACKIDX:.*]] = arith.subi %[[LEN]], %[[C1]] : index
// CHECK: emitrust.subscript %{{.*}}[%[[BACKIDX]]] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>, index) -> !emitrust.lvalue<i32>
// CHECK: emitrust.method_call %{{.*}}["pop"] () : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>) -> ()
int vector_ends(void) {
  std::vector<int> v;
  v.push_back(3);
  v.push_back(9);
  int f = v.front();
  int b = v.back();
  v.pop_back();
  return f + b;
}

// CHECK-LABEL: func.func @string_grow
// CHECK: emitrust.method_call %{{.*}}["push"] (%{{.*}}) : (!emitrust.lvalue<!emitrust.opaque<"String">>, !emitrust.opaque<"char">) -> ()
// CHECK: emitrust.method_call %{{.*}}["clear"] () : (!emitrust.lvalue<!emitrust.opaque<"String">>) -> ()
int string_grow(void) {
  std::string s = "x";
  s.push_back('y');
  int n = s.size();
  s.clear();
  return n + s.empty();
}
