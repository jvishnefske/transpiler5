// RUN: emitrust-import-c %s | FileCheck %s

// CTS-S6: integer-to-enum conversion (the reverse of C99-5's enum-to-int
// direction). C's enum objects hold any value of the underlying type, not
// just declared enumerators (C99 6.7.2.2), so the conversion lowers to a
// value-preserving `emitrust.cast` to the enum type; a reference to an
// enumerator of the destination enum keeps the direct constant fast path.
// The enum definition is emitted once per module no matter how many casts
// target it, and clang's unsigned underlying-type choice (all enumerator
// values non-negative) is recorded as the `unsigned_underlying` marker.
// C's enum/underlying-type pointer compatibility (`&e` passed as
// `unsigned int *`) borrows the raw-representation place through
// `emitrust.enum_raw`.

enum Fred { A, B, C = 54 };

unsigned int deref(unsigned int *p) {
  return *p;
}

int roundtrip(int raw) {
  enum Fred f;
  f = raw;
  f = 12;
  f = C;
  unsigned int u = deref(&f);
  return (int)f + (int)u;
}

// CHECK: emitrust.enum_def @Fred ["A", "B", "C"] [0, 1, 54] {unsigned_underlying}
// CHECK-NOT: emitrust.enum_def @

// CHECK-LABEL: func.func @roundtrip

// The declaration takes a variable place of the enum type.
// CHECK: %[[F:.*]] = emitrust.variable named "f" : !emitrust.lvalue<!emitrust.enum<"Fred">>

// `f = raw`: an arbitrary int converts through a value-preserving cast.
// CHECK: %[[RAW:.*]] = emitrust.cast %{{[0-9]+}} : i32 to !emitrust.enum<"Fred">
// CHECK: emitrust.assign %[[F]] = %[[RAW]]

// `f = 12`: 12 is not a declared enumerator; the value is preserved, not
// diagnosed (C's conversion to the underlying type).
// CHECK: %[[TWELVE:.*]] = emitrust.cast %{{[a-z0-9_]+}} : i32 to !emitrust.enum<"Fred">
// CHECK: emitrust.assign %[[F]] = %[[TWELVE]]

// `f = C`: an enumerator of the destination enum keeps the constant path.
// CHECK: %[[CONST:.*]] = emitrust.constant <#emitrust.opaque<"Fred::C">> : !emitrust.enum<"Fred">
// CHECK: emitrust.assign %[[F]] = %[[CONST]]

// `deref(&f)`: the address of the enum object passed as `unsigned int *`
// borrows the raw-representation place at the storage type.
// CHECK: %[[RAWPLACE:.*]] = emitrust.enum_raw %[[F]] : (!emitrust.lvalue<!emitrust.enum<"Fred">>) -> !emitrust.lvalue<ui32>
// CHECK: %[[REF:.*]] = emitrust.addr_of mut %[[RAWPLACE]] : (!emitrust.lvalue<ui32>) -> !emitrust.mut_ref<ui32>
// CHECK: call @deref(%[[REF]])
