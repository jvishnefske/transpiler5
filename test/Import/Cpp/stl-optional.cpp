// RUN: emitrust-import-c %s | FileCheck %s
// W2.11: std::optional<T> imports as !emitrust.opaque<"Option<S>"> —
// Rust's Option<T> IS the semantic model (engaged/empty), so no
// synthesized struct is needed. Pins the chosen lowerings at the IR
// level: the element converting ctor (`return v;` — a value-position
// CXXConstructExpr intercepted BEFORE the generic trivial-copy unwrap)
// renders `call_opaque "Some"(v)`; the default ctor and the
// std::nullopt_t converting ctor both render `literal "None"`; a local
// initialized from a bare CallExpr (C++17 guaranteed elision — NO ctor
// wrapper exists in the AST) stores the loaded rvalue whole;
// has_value() -> method_call "is_some" (i1) and value_or(d) ->
// method_call "unwrap_or" (element-typed) — identical semantics on both
// sides, both by value. value()/operator*/copy construction stay
// located rejections (stl-invalid.cpp pins that frontier).

extern "C" int printf(const char *, ...);

#include <optional>

// CHECK-LABEL: func.func @find_even
// CHECK-SAME: -> !emitrust.opaque<"Option<i32>">
// The engaged return: `return v;` converts through the element
// converting ctor -> `Some(v)`.
// CHECK: %[[SOME:.*]] = emitrust.call_opaque "Some"(%{{.*}}) : (i32) -> !emitrust.opaque<"Option<i32>">
// CHECK: return %[[SOME]] : !emitrust.opaque<"Option<i32>">
// The empty return: `return std::nullopt;` converts through the
// nullopt_t ctor -> `None`.
// CHECK: %[[NONE:.*]] = emitrust.literal "None" : !emitrust.opaque<"Option<i32>">
// CHECK: return %[[NONE]] : !emitrust.opaque<"Option<i32>">
std::optional<int> find_even(int v) {
  if (v % 2 == 0)
    return v;
  return std::nullopt;
}

// CHECK-LABEL: func.func @use_optional
// The call-initialized local: a bare CallExpr initializer (guaranteed
// elision), stored whole into the variable place.
// CHECK: %[[A:.*]] = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.opaque<"Option<i32>">>
// CHECK: %[[CALL:.*]] = call @find_even(%{{.*}}) : (i32) -> !emitrust.opaque<"Option<i32>">
// CHECK: emitrust.assign %[[A]] = %[[CALL]] : !emitrust.lvalue<!emitrust.opaque<"Option<i32>">>
// The default-constructed local -> `None`.
// CHECK: %[[O:.*]] = emitrust.variable named "o" : !emitrust.lvalue<!emitrust.opaque<"Option<i32>">>
// CHECK: %[[ONONE:.*]] = emitrust.literal "None" : !emitrust.opaque<"Option<i32>">
// CHECK: emitrust.assign %[[O]] = %[[ONONE]] : !emitrust.lvalue<!emitrust.opaque<"Option<i32>">>
// has_value() -> is_some, a genuine i1 (the (int) cast is the ordinary
// later conversion).
// CHECK: %[[HAS:.*]] = emitrust.method_call %[[A]]["is_some"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<i32>">>) -> i1
// value_or(-1) -> unwrap_or, element-typed argument and result.
// CHECK: emitrust.method_call %[[A]]["unwrap_or"] (%{{.*}}) : (!emitrust.lvalue<!emitrust.opaque<"Option<i32>">>, i32) -> i32
int use_optional(void) {
  std::optional<int> a = find_even(8);
  std::optional<int> o;
  int engaged = (int)a.has_value();
  int value = a.value_or(-1);
  return engaged + value + (int)o.has_value();
}
