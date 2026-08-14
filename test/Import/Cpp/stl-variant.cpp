// RUN: emitrust-import-c %s | FileCheck %s
// W2.14: std::variant<int, double> imports as a SYNTHESIZED closed
// two-variant Rust data enum (`emitrust.data_enum_def @Variant_i32_f64`,
// route (a) of the W2.14 spike — no Rust std variant image exists, and
// the data-enum dialect machinery renders today). Pins the chosen
// lowerings at the IR level: the converting ctor from an alternative
// value renders `enum_variant "V0"/"V1"` + assign into the local's
// `emitrust.variable` place (a data enum deliberately derives no
// Default, so the place carries NO init attribute and construction is
// always an explicit assign); the DEFAULT ctor renders the EXPLICIT
// `V0 { 0 }` image (C++17 [variant.ctor]p2 value-initializes the first
// alternative — never the emitter's default-value path); `index()`
// expands to a RESULT-mode exhaustive `emitrust.match` minting i32
// constants per arm (cast up to the call's declared size_t for
// fidelity); `std::get<T>(v)` — a FREE function returning `T&`,
// intercepted at the value read since no place for the result exists —
// expands to a match yielding the held payload, with a diverging
// `panic!` image in the other arm (behavior-compatible for the
// supported subset: catch is unsupported, so no program can observe
// the C++ exception instead); operator= re-tags the place with a fresh
// enum_variant. >2/duplicate/non-scalar alternatives,
// holds_alternative, visit, get<index>, and valueless_by_exception
// stay located rejections (stl-invalid.cpp pins that frontier).

extern "C" int printf(const char *, ...);

#include <variant>

// CHECK-LABEL: func.func @use_variant
// The converting ctor from a runtime int value -> V0 + assign.
// CHECK: %[[V:.*]] = emitrust.variable named "v" : !emitrust.lvalue<!emitrust.data_enum<"Variant_i32_f64">>
// CHECK: %[[E0:.*]] = emitrust.enum_variant @Variant_i32_f64 "V0"(%{{.*}}) : (i32) -> !emitrust.data_enum<"Variant_i32_f64">
// CHECK: emitrust.assign %[[V]] = %[[E0]]
// index() -> RESULT-mode exhaustive match, i32 constants per arm, cast
// up to the call's declared size_t (ui64).
// CHECK: %[[L0:.*]] = emitrust.load %[[V]]
// CHECK: %[[IDX:.*]] = emitrust.match %[[L0]] : !emitrust.data_enum<"Variant_i32_f64"> -> i32
// CHECK: case "V0" (%{{.*}}: i32)
// CHECK: case "V1" (%{{.*}}: f64)
// CHECK: emitrust.cast %[[IDX]] : i32 to ui64
// std::get<int>(v) -> match yielding the held payload; the OTHER arm is
// the diverging panic! image.
// CHECK: emitrust.match %{{.*}} : !emitrust.data_enum<"Variant_i32_f64"> -> i32
// CHECK: case "V0" (%[[P0:.*]]: i32)
// CHECK: emitrust.yield %[[P0]] : i32
// CHECK: case "V1" (%{{.*}}: f64)
// CHECK: emitrust.call_opaque "panic!"() {args = ["std::get: wrong variant alternative"]}
// v = 2.5 -> operator= re-tags the SAME place with a fresh V1 value.
// CHECK: %[[E1:.*]] = emitrust.enum_variant @Variant_i32_f64 "V1"(%{{.*}}) : (f64) -> !emitrust.data_enum<"Variant_i32_f64">
// CHECK: emitrust.assign %[[V]] = %[[E1]]
// std::get<double>(v) -> the f64-held match; now V0 is the panic arm.
// CHECK: emitrust.match %{{.*}} : !emitrust.data_enum<"Variant_i32_f64"> -> f64
// CHECK: case "V0" (%{{.*}}: i32)
// CHECK: emitrust.call_opaque "panic!"() {args = ["std::get: wrong variant alternative"]}
// CHECK: case "V1" (%[[P1:.*]]: f64)
// CHECK: emitrust.yield %[[P1]] : f64
// The default ctor: the EXPLICIT first-alternative zero image V0 { 0 }.
// CHECK: %[[W:.*]] = emitrust.variable named "w" : !emitrust.lvalue<!emitrust.data_enum<"Variant_i32_f64">>
// CHECK: %[[EW:.*]] = emitrust.enum_variant @Variant_i32_f64 "V0"(%{{.*}}) : (i32) -> !emitrust.data_enum<"Variant_i32_f64">
// CHECK: emitrust.assign %[[W]] = %[[EW]]
int use_variant(int a) {
  std::variant<int, double> v = a;
  int idx0 = v.index();
  int held = std::get<int>(v);
  v = 2.5;
  double d = std::get<double>(v);
  std::variant<int, double> w;
  int widx = w.index();
  return idx0 + held + widx + (int)d;
}

// The one module-level definition every mention shares: shape-keyed
// through typeRustName — verbatim `Variant_i32_f64` under this tool's
// name-preserving default, `VariantI32F64` under emitrust-cc's idiomatic
// rename (the spelling the crate's non_camel_case_types deny accepts;
// rustc rejects design.md's `__Variant_i32_f64` sketch there). Brace
// variants with the one payload field `v` each — the only form
// data_enum_def supports.
// CHECK: emitrust.data_enum_def @Variant_i32_f64 ["V0", "V1"] {{\[\[}}"v"], {{\[}}"v"]] {{\[\[}}i32], {{\[}}f64]]
