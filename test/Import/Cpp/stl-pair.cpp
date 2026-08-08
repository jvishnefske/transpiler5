// RUN: emitrust-import-c %s | FileCheck %s
// W2.8: std::pair<T1, T2> imports as an importer-SYNTHESIZED real struct
// (task-005 spike route (b)): one shape-keyed struct_def per
// instantiation (`Pair_i32_i32`, `Pair_i32_f64` — pre-seeded names, so
// the shared clang spelling `pair` cannot collide), fields exactly
// first/second. Pins: the two-argument value constructor assigning the
// two member places field-wise (no libc++ ctor is ever imported — the
// std-record method walk is skipped wholesale); member read AND
// compound-assign write through ordinary emitrust.member places; a
// value-position construction (`return std::pair<int,int>(...)`)
// materializing an anonymous temp place, field-initializing it, and
// loading it whole for the by-value struct return.

extern "C" int printf(const char *, ...);

#include <utility>

// CHECK: emitrust.struct_def @Pair_i32_i32 ["first", "second"] [i32, i32]

// CHECK-LABEL: func.func @divmod
// CHECK-SAME: -> !emitrust.struct<"Pair_i32_i32">
// CHECK: %[[TMP:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Pair_i32_i32">>
// CHECK: %[[F:.*]] = emitrust.member %[[TMP]]["first"]
// CHECK: emitrust.assign %[[F]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[S:.*]] = emitrust.member %[[TMP]]["second"]
// CHECK: emitrust.assign %[[S]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[WHOLE:.*]] = emitrust.load %[[TMP]] : (!emitrust.lvalue<!emitrust.struct<"Pair_i32_i32">>) -> !emitrust.struct<"Pair_i32_i32">
// CHECK: return %[[WHOLE]]
std::pair<int, int> divmod(int num, int den) {
  return std::pair<int, int>(num / den, num % den);
}

// CHECK-LABEL: func.func @use_pair
// CHECK: %[[P:.*]] = emitrust.variable named "p" : !emitrust.lvalue<!emitrust.struct<"Pair_i32_i32">>
// CHECK: %[[PF:.*]] = emitrust.member %[[P]]["first"]
// CHECK: emitrust.assign %[[PF]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[PS:.*]] = emitrust.member %[[P]]["second"]
// CHECK: emitrust.assign %[[PS]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: emitrust.variable named "mixed" : !emitrust.lvalue<!emitrust.struct<"Pair_i32_f64">>
int use_pair(void) {
  std::pair<int, int> p(3, 40);
  p.first += 2;
  std::pair<int, double> mixed(1, 2.5);
  return p.first + p.second + mixed.first;
}
