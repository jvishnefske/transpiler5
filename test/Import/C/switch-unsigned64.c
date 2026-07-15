// RUN: emitrust-import-c %s | FileCheck %s
//
// This test deliberately has no `| emitrust-opt` reparse pipe: upstream
// cf.switch prints case values unsigned while its parser accepts only
// signed i64, so a label above i64::MAX does not survive a textual
// round-trip. The in-memory emitrust-cc pipeline is unaffected;
// test/EndToEnd/unsigned.c verifies this switch differentially against a
// native clang build.

int on_ulonglong(unsigned long long v) {
  // The ui64 scrutinee reinterprets its bit pattern into signless i64
  // (`as i64`); the case label 0xFFFFFFFFFFFFFFFE zero-extends to the same
  // 64 bits, so scrutinee and label agree bit for bit even above i64::MAX.
  switch (v) {
  case 0xFFFFFFFFFFFFFFFEull:
    return 1;
  case 5ull:
    return 2;
  default:
    return 0;
  }
}

// CHECK-LABEL: func.func @on_ulonglong
// CHECK: emitrust.cast %{{.*}} : ui64 to i64
// CHECK: cf.switch %{{[0-9]+}} : i64, [
// CHECK-NEXT: default: ^bb{{[0-9]+}},
// CHECK-NEXT: 18446744073709551614: ^bb{{[0-9]+}},
// CHECK-NEXT: 5: ^bb{{[0-9]+}}
// CHECK-NEXT: ]
