// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/keyword-name.cpp 2>&1 | FileCheck %s --check-prefix=KWNAME
// RUN: not emitrust-import-c %t/big-value.cpp 2>&1 | FileCheck %s --check-prefix=BIGVAL
// RUN: not emitrust-import-c %t/huge-value.cpp 2>&1 | FileCheck %s --check-prefix=HUGEVAL
// RUN: not emitrust-import-c %t/empty.cpp 2>&1 | FileCheck %s --check-prefix=EMPTY
// RUN: not emitrust-import-c %t/nonliftable-default.cpp 2>&1 | FileCheck %s --check-prefix=NLDEFAULT

// FR-113 RETIRED the blanket scoped-enumeration rejection this file's first
// two legs used to pin: `enum class`/`enum struct` now import as their
// underlying-typed C-like image (test/Import/Cpp/cpp-enum-class.cpp pins
// the admission). What this file pins instead is that the RESIDUAL enum
// definition gates -- keyword-named enums, values outside the underlying
// type's range, an empty enum (expressible only in C++) -- still reject
// scoped definitions with the same LOCATED wordings the unscoped gates
// carry, at the definition, not as a downstream type mismatch. The
// NLDEFAULT leg is unrelated to enums and survives from the original
// Stage-D file: a defaulted argument the importer cannot lift rejects with
// a real file:line:col location (recursed into the default value
// expression).
//
// FR-166 MOVED THE RANGE PIN FORWARD, it did not loosen it. A FIXED
// underlying type of `long long` is now ADMITTED whole (that is the whole
// point of the feature -- systemd's `_SD_ENUM_FORCE_S64` shape), so the leg
// that used to spell `enum class Big : long long { V = 3000000000LL }` is
// replaced by the two gates that survive it:
//   * BIGVAL -- a 32-BIT fixed underlying type still bounds its values by
//     i32. `unsigned int` is deliberate: the enum-to-integer conversion
//     still drops signedness, so an admitted UINT32_MAX enumerator would
//     MISCOMPILE (FR-169), not merely widen.
//   * HUGEVAL -- a 64-bit fixed underlying type is admitted, but a value
//     above INT64_MAX exceeds `DenseI64ArrayAttr` and stays rejected with
//     the i64 wording.

//--- keyword-name.cpp
// KWNAME: keyword-name.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enum name 'match' is a Rust keyword
enum class match { First, Second };
int use(match m) {
  return static_cast<int>(m);
}

//--- big-value.cpp
// BIGVAL: big-value.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator value does not fit in i32
enum class Big : unsigned int { V = 3000000000u };
int use(Big b) {
  return 0;
}

//--- huge-value.cpp
// HUGEVAL: huge-value.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator value does not fit in i64
enum class Huge : unsigned long long { V = 18446744073709551615ull };
int use(Huge h) {
  return 0;
}

//--- empty.cpp
// EMPTY: empty.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enum with no enumerators
enum class Empty {};
int use(Empty e) {
  return 0;
}

//--- nonliftable-default.cpp
struct Widget {
  int x;
};
// The default value is a value-position construction, which does not lift; the
// rejection must now carry the default expression's own location.
// NLDEFAULT: nonliftable-default.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: constructor in value position (only a trivial copy or move is modeled)
int take(int a, Widget w = Widget()) {
  return a + w.x;
}
int use() {
  return take(5);
}
