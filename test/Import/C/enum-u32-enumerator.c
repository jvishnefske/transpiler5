// RUN: emitrust-import-c %s | FileCheck %s

// FR-166 phase 2 + FR-169 phase C. Pins the two halves of admitting a
// 32-bit UNSIGNED-underlying enum whose enumerators leave the `int` range.
//
// Half one (FR-166 phase 2): the importer's enumerator range gate is
// `isUInt<32>` when the underlying type is unsigned and `isInt<32>` when it
// is signed, instead of `isInt<32>` for both. `M_MID`/`M_MAX` are admitted;
// before this they were a located "enumerator value does not fit in i32"
// rejection that took the whole enum, and everything naming it, down.
//
// Half two (FR-169 phase C): the type an ENUMERATOR REFERENCE is normalized
// to is `mapType(ref->getType())` -- the type CLANG gave the DeclRefExpr --
// not a hard i32. Clang gives it the enum's PROMOTION type, which is `int`
// whenever every enumerator fits `int` (the C17 6.4.4.3 rule, and the case
// that stays byte-identical), and the unsigned/wider underlying type
// otherwise. Forcing THAT through i32 turns 4294967295 into -1 the moment
// it is widened: `(unsigned long)M_MAX` was measured printing
// 18446744073709551615.
//
// The two halves must land together. Half one alone is a silent miscompile
// in one direction (`(unsigned long)M_MAX`) and a spurious "assigned value
// type does not match the place" rejection in the other (`unsigned a =
// M_MAX;`), so the failure direction is not even uniformly safe.
//
// `enum S` (a negative enumerator, so a SIGNED underlying type) and
// `enum E` (all enumerators in `int` range, so promotion type `int`) are
// side-by-side non-regression pins: neither change moves a byte for them,
// and between them they cover every enum the importer admitted before.

enum M { M_A = 1, M_MID = 2147483648u, M_MAX = 4294967295u };
enum S { S_N = -1, S_A = 1 };
enum E { E_A = 1, E_B = 2 };
enum WU { WU_A = 0, WU_B = 5000000000ULL };

// The u32-range enum is admitted, with its values recorded verbatim.
// CHECK: emitrust.enum_def @M ["M_A", "M_MID", "M_MAX"] [1, 2147483648, 4294967295] {unsigned_underlying}
// CHECK: emitrust.enum_def @S ["S_N", "S_A"] [-1, 1]
// CHECK: emitrust.enum_def @E ["E_A", "E_B"] [1, 2] {unsigned_underlying}
// CHECK: emitrust.enum_def @WU ["WU_A", "WU_B"] [0, 5000000000] {unsigned_underlying, wide_underlying}

// `enum M`'s promotion type is `unsigned int`, so every one of its
// enumerator references normalizes to ui32 and widens ZERO-extending.
// This is the leg that miscompiled.
// CHECK-LABEL: func.func @max_to_ulong
// CHECK: %[[C:.*]] = emitrust.constant <#emitrust.opaque<"M::M_MAX">> : !emitrust.enum<"M">
// CHECK: %[[U:.*]] = emitrust.cast %[[C]] : !emitrust.enum<"M"> to ui32
// CHECK: emitrust.cast %[[U]] : ui32 to ui64
// CHECK-NOT: to i32
unsigned long max_to_ulong(void) { return (unsigned long)M_MAX; }

// An IN-RANGE enumerator of that same enum takes the same ui32 route,
// because the promotion type is a property of the ENUM, not of the
// individual enumerator -- and 1 has the same value either way.
// CHECK-LABEL: func.func @a_to_ulong
// CHECK: %[[CA:.*]] = emitrust.constant <#emitrust.opaque<"M::M_A">> : !emitrust.enum<"M">
// CHECK: %[[UA:.*]] = emitrust.cast %[[CA]] : !emitrust.enum<"M"> to ui32
// CHECK: emitrust.cast %[[UA]] : ui32 to ui64
unsigned long a_to_ulong(void) { return (unsigned long)M_A; }

// An enumerator initializing a plain `unsigned`, direct and through
// arithmetic. Both are a located rejection ("assigned value type does not
// match the place") if the range is opened and the reference is still
// forced through i32.
// CHECK-LABEL: func.func @plain_unsigned
// CHECK: emitrust.assign %{{[0-9]+}} = %{{[0-9]+}} : !emitrust.lvalue<ui32>
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"M"> to ui32
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"M"> to ui32
// CHECK: emitrust.sub %{{[0-9]+}}, %{{[0-9]+}} : ui32
unsigned plain_unsigned(void) {
  unsigned a = M_MAX;
  unsigned c = M_MAX - M_A;
  return a + c;
}

// Non-regression: an enum with a SIGNED underlying type is untouched --
// its promotion type is `int`, so `mapType` hands back the same i32 the
// hard cast used to.
// CHECK-LABEL: func.func @signed_untouched
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"S"> to i32
// CHECK-NOT: to ui32
long signed_untouched(void) { return (long)S_N; }

// Non-regression: an all-in-range unsigned-underlying enum is untouched.
// Clang's promotion type for it is `int` even though its STORAGE is
// `unsigned int`, so this is the shape that would have shifted bytes for
// every non-negative enum in the corpus had the rule been driven off the
// underlying type instead of off the reference's type.
// CHECK-LABEL: func.func @inrange_untouched
// CHECK: emitrust.cast %{{[0-9]+}} : !emitrust.enum<"E"> to i32
// CHECK-NOT: to ui32
unsigned long inrange_untouched(void) { return (unsigned long)E_B; }

// FR-166's 64-bit unsigned storage follows the same rule at ui64. This one
// DOES move: it used to normalize to a signless i64 (`castEnumToI32`
// widens for `wide_underlying` but has no signedness to widen WITH), so
// `(int)WU_B` was `as i64 as i32`. Every admitted wide value is <=
// INT64_MAX, so the bits agree either way; the point of pinning it is that
// the signedness of an enumerator reference now comes from one place.
// CHECK-LABEL: func.func @wide_unsigned
// CHECK: %[[CW:.*]] = emitrust.constant <#emitrust.opaque<"WU::WU_B">> : !emitrust.enum<"WU">
// CHECK: %[[UW:.*]] = emitrust.cast %[[CW]] : !emitrust.enum<"WU"> to ui64
// CHECK: emitrust.cast %[[UW]] : ui64 to i32
int wide_unsigned(void) { return (int)WU_B; }
