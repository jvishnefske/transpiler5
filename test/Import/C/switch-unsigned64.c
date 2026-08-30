// A `switch` over an `unsigned long long` with a case label ABOVE i64::MAX.
//
// The ui64 scrutinee reinterprets its bit pattern into signless i64 (`as
// i64`); the case label 0xFFFFFFFFFFFFFFFE zero-extends to the same 64 bits,
// so scrutinee and label agree bit for bit even above i64::MAX.
//
// FR-135: this file USED to carry the note "deliberately has no
// `| emitrust-opt` reparse pipe", because upstream's cf.switch prints case
// labels unsigned (`APInt::getLimitedValue()`) while its own parser reads
// them with `parseInteger(int64_t)`, so the printed module died on re-read
// with `custom op 'cf.switch' integer value too large`. Our IR was correct
// the whole time -- it is the CUSTOM assembly that cannot spell this label.
//
// The importer now prints such a module in MLIR's GENERIC form, which is
// lossless, and says so with a located remark. So the reparse pipe this file
// once documented as impossible is now part of the test, and the checks pin
// the generic spelling of the very same two labels: `dense<[-2, 5]>`, where
// -2 as i64 IS 0xFFFFFFFFFFFFFFFE. test/EndToEnd/unsigned.c still verifies
// the switch differentially against a native clang build; the in-memory
// emitrust-cc pipeline was never affected by any of this.
//
// RUN: emitrust-import-c %s -o %t.mlir 2>%t.err
// RUN: FileCheck %s < %t.mlir
// RUN: FileCheck %s --check-prefix=REMARK < %t.err
// RUN: emitrust-opt %t.mlir -o /dev/null

int on_ulonglong(unsigned long long v) {
  // The remark is located at the SWITCH, which is where the cf.switch op is.
  // REMARK: switch-unsigned64.c:[[#@LINE+1]]:3: remark: switch case value 18446744073709551614 has no round-trippable spelling in the custom 'cf.switch' assembly, so this module is printed in MLIR's generic form
  switch (v) {
  case 0xFFFFFFFFFFFFFFFEull:
    return 1;
  case 5ull:
    return 2;
  default:
    return 0;
  }
}

// REMARK: note: upstream cf.switch prints case values unsigned and parses them signed, so the custom form would not re-read; the generic form is lossless

// CHECK: "func.func"() <{function_type = (ui64) -> i32, sym_name = "on_ulonglong"}>
// CHECK: "emitrust.cast"(%{{[0-9]+}}) : (ui64) -> i64
// The first successor is the DEFAULT destination, then one per case label in
// order; the labels themselves are the `case_values` attribute.
// CHECK: "cf.switch"(%{{[0-9]+}})[^bb{{[0-9]+}}, ^bb{{[0-9]+}}, ^bb{{[0-9]+}}]
// CHECK-SAME: case_values = dense<[-2, 5]> : vector<2xi64>

// The unreadable custom spelling must not be what we wrote out.
// CHECK-NOT: 18446744073709551614
