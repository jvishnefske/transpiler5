// RUN: split-file %s %t
// RUN: emitrust-import-c %t/positive.cpp | FileCheck %s
// RUN: not emitrust-import-c %t/by-reference.cpp 2>&1 | FileCheck %s --check-prefix=BYREF
// RUN: not emitrust-import-c %t/tuple-protocol.cpp 2>&1 | FileCheck %s --check-prefix=TUPLEPROTO
// W2.9: BY-VALUE structured bindings desugar to an anonymous holding copy
// plus one ordinary scalar local per binding (each binding IS a member of
// the hidden copy nothing else can alias, so separate locals differ only
// in address identity, which the subset cannot observe). Pins: the
// holding place assigned the whole source value, each binding local
// initialized from the zipped member (struct/pair) or element
// (std::array), and the two rejections whose desugar would MISCOMPILE if
// accepted: the reference form (`auto &[..]` aliases the SOURCE — writes
// through a per-binding copy would be lost) and a user type binding
// through a custom tuple-like protocol (clang binds via get<i> calls, not
// the zipped fields).

//--- positive.cpp
extern "C" int printf(const char *, ...);

#include <utility>
#include <array>

struct Dim {
  int w;
  int h;
};

// CHECK-LABEL: func.func @over_struct
// CHECK: %[[SRC:.*]] = emitrust.load %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"Dim">>) -> !emitrust.struct<"Dim">
// CHECK: %[[HOLD:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Dim">>
// CHECK: emitrust.assign %[[HOLD]] = %[[SRC]]
// CHECK: %[[WM:.*]] = emitrust.member %[[HOLD]]["w"]
// CHECK: %[[WV:.*]] = emitrust.load %[[WM]]
// CHECK: %[[W:.*]] = emitrust.variable named "w" : !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[W]] = %[[WV]]
// CHECK: %[[HM:.*]] = emitrust.member %[[HOLD]]["h"]
// CHECK: emitrust.variable named "h"
int over_struct(void) {
  Dim d;
  d.w = 12;
  d.h = 5;
  auto [w, h] = d;
  return w * h;
}

// CHECK-LABEL: func.func @over_pair
// CHECK: %[[PHOLD:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Pair_i32_i32">>
// CHECK: emitrust.member %[[PHOLD]]["first"]
// CHECK: emitrust.variable named "q"
// CHECK: emitrust.member %[[PHOLD]]["second"]
// CHECK: emitrust.variable named "r"
int over_pair(void) {
  std::pair<int, int> p(6, 9);
  auto [q, r] = p;
  return q + r;
}

// CHECK-LABEL: func.func @over_array
// CHECK: %[[AHOLD:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<3xi32>>
// CHECK: %[[I0:.*]] = arith.constant 0 : index
// CHECK: emitrust.subscript %[[AHOLD]][%[[I0]]]
// CHECK: emitrust.variable named "x"
// CHECK: emitrust.subscript %[[AHOLD]]
// CHECK: emitrust.variable named "y"
// CHECK: emitrust.subscript %[[AHOLD]]
// CHECK: emitrust.variable named "z"
int over_array(void) {
  std::array<int, 3> a = {7, 21, 35};
  auto [x, y, z] = a;
  return x + y + z;
}

//--- by-reference.cpp
struct Dim {
  int w;
  int h;
};
// BYREF: by-reference.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: structured binding by reference
int use(void) {
  Dim d;
  d.w = 1;
  d.h = 2;
  auto &[w, h] = d;
  w = 9;
  return d.w;
}

//--- tuple-protocol.cpp
// A user type opting into the tuple-like protocol binds through get<i>
// calls, and the field-zip desugar would read the WRONG data (fields a/b
// vs get's b/a swap below) — emitDecompositionDecl carries a
// binding-is-the-zipped-field guard for exactly this. TODAY the guard is
// unreachable belt-and-braces: opting in requires a std::tuple_size
// specialization at file scope, and that template declaration already
// rejects as an unsupported top-level declaration (pinned here) before
// any binding imports. Authoring this file also flushed out TWO crashes
// on the way to that rejection: a class template partial specialization
// (IS-A RecordDecl) reaching importRecord/isByteRegionRecord as a
// DEPENDENT type used to blow the stack in clang's getTypeInfo instead
// of rejecting; both sites now guard on isDependentType().
#include <utility>
using std::size_t;
struct Swapped {
  int a;
  int b;
};
// TUPLEPROTO: tuple-protocol.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported top-level declaration
template <> struct std::tuple_size<Swapped> {
  static constexpr size_t value = 2;
};
template <size_t I> struct std::tuple_element<I, Swapped> {
  using type = int;
};
template <size_t I> int get(const Swapped &s) { return I == 0 ? s.b : s.a; }
int use(void) {
  Swapped s;
  s.a = 1;
  s.b = 2;
  auto [x, y] = s;
  return x;
}
