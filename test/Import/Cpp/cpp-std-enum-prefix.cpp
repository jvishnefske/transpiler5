// The `std::` half of FR-108's enum arm, pinned because it is a
// deliberate DIVERGENCE from the record path rather than an oversight.
//
// A record in namespace `std` never reaches `recordRustName`: `mapType`
// diverts a `RecordType` whose decl `isInStdNamespace()` to
// `mapStdLibraryType` before `importRecord` runs, so `std::pair<int,int>`
// stays `PairI32I32` with no prefix. There is NO such arm for `EnumType`
// -- an enum in namespace `std` flows through the ordinary import -- so it
// now takes the ordinary `ns_std_` prefix.
//
// That is the answer this file pins, and it is the safe one: unprefixed,
// a user-written `enum float_round_style` would compose onto libstdc++'s
// `std::float_round_style` and the shape dedup would either refuse the
// program or (same shape) merge the two into one Rust type. The COLLIDE
// leg is that property stated as behavior -- the user's enum and the
// library's coexist, with different variant sets, under two names.
//
// `std::float_round_style` is chosen because it is a plain unscoped enum
// declared directly in namespace `std` with a fixed, standard-mandated
// enumerator list, so the check is not sensitive to libstdc++ internals.
// RUN: emitrust-cc --emit=rust %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=PRESERVED

#include <limits>

extern "C" int printf(const char *, ...);

// A hand-written enum whose C spelling matches the library's exactly.
enum float_round_style { user_only = 41 };

int main(int argc, char **argv) {
  std::float_round_style lib = std::round_toward_zero;
  float_round_style mine = user_only;
  printf("%d %d\n", (int)lib + argc, (int)mine + argc);
  return 0;
}

// Two enums, two Rust types, two variant sets: the library's carries the
// namespace prefix and the user's -- at global scope -- does not.
// CHECK-DAG: struct NsStdFloatRoundStyle(i32);
// CHECK-DAG: struct FloatRoundStyle(u32);
// CHECK-DAG: const ROUND_TOWARD_ZERO: NsStdFloatRoundStyle = NsStdFloatRoundStyle(0);
// CHECK-DAG: const USER_ONLY: FloatRoundStyle = FloatRoundStyle(41);

// Rename OFF: the prefix is the whole change; the C spellings are kept,
// and the two therefore differ in exactly the prefix.
// PRESERVED-DAG: emitrust.enum_def @ns_std_float_round_style
// PRESERVED-DAG: emitrust.enum_def @float_round_style ["user_only"] [41]
