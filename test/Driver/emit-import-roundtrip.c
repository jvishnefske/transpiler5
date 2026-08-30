// FR-135: `--emit=import` output must be re-readable by `emitrust-opt`.
//
// `--emit=import` exists for ONE reason: to hand a person (or a bisecting
// script) the front end's module so it can be re-read and poked at. It was
// not always re-readable. Measured at HEAD before this change, on
// test/EndToEnd/switch-general.c:
//
//   error: custom op 'cf.switch' integer value too large
//         18446744073709551615: ^bb1,
//
// The defect is UPSTREAM and our IR is CORRECT. MLIR's `cf.switch` custom
// printer emits each case label with `APInt::getLimitedValue()`, i.e.
// UNSIGNED (mlir/lib/Dialect/ControlFlow/IR/ControlFlowOps.cpp,
// `printSwitchOpCases`), while its own `parseSwitchOpCases` reads labels
// with `parser.parseInteger(int64_t)`. Any label whose zero-extension
// exceeds `i64::MAX` therefore prints as a decimal its own parser refuses.
// The GENERIC form of the very same op prints `case_values = dense<-2>` and
// round-trips exactly.
//
// So the fix is to print the module in MLIR's generic form -- the form that
// exists precisely to be lossless -- whenever it contains a label the custom
// form cannot spell, and to SAY SO with a located remark rather than
// silently changing the output shape. MLIR's printing flags are module-wide,
// not per-op, so the fallback is all-or-nothing for the module; the measured
// blast radius over the whole test corpus plus third_party/c-testsuite was
// THREE units (test/EndToEnd/switch-general.c, test/EndToEnd/unsigned.c and
// test/Import/C/switch-unsigned64.c), and exactly one golden --
// test/Import/C/switch-unsigned64.c -- checks the printed form. That golden
// gained the `| emitrust-opt` reparse it previously documented as
// impossible.
//
// Both halves are pinned here: the lossy module must go generic, carry the
// remark, and re-parse; and an ordinary module must be BYTE-UNCHANGED --
// custom form, no remark -- so the fallback cannot creep into the common
// path. test/Driver/emit-mlir.c holds the ordinary custom-form shape too.
//
// RUN: split-file %s %t
//
// The lossy module: generic form, a located remark, and a clean reparse.
// RUN: emitrust-cc --emit=import %t/lossy.c -o %t/lossy.mlir 2>%t/lossy.err
// RUN: FileCheck %s --check-prefix=REMARK < %t/lossy.err
// RUN: FileCheck %s --check-prefix=GENERIC < %t/lossy.mlir
// RUN: emitrust-opt %t/lossy.mlir -o /dev/null
//
// The ordinary module: custom form, no remark, and it re-parses as it always
// did.
// RUN: emitrust-cc --emit=import %t/plain.c -o %t/plain.mlir 2>%t/plain.err
// RUN: FileCheck %s --check-prefix=PLAIN < %t/plain.mlir
// RUN: FileCheck %s --check-prefix=NOREMARK --allow-empty < %t/plain.err
// RUN: emitrust-opt %t/plain.mlir -o /dev/null
//
// The same guarantee holds for the standalone importer tool, which prints
// the same module.
// RUN: emitrust-import-c %t/lossy.c -o %t/lossy2.mlir 2>%t/lossy2.err
// RUN: FileCheck %s --check-prefix=REMARK < %t/lossy2.err
// RUN: emitrust-opt %t/lossy2.mlir -o /dev/null

//--- lossy.c
int on_ulonglong(unsigned long long v) {
  switch (v) {
  case 0xFFFFFFFFFFFFFFFEull:
    return 1;
  case 5ull:
    return 2;
  default:
    return 0;
  }
}

// REMARK: lossy.c:{{[0-9]+}}:{{[0-9]+}}: remark: switch case value 18446744073709551614 has no round-trippable spelling in the custom 'cf.switch' assembly, so this module is printed in MLIR's generic form
// REMARK: note: upstream cf.switch prints case values unsigned and parses them signed, so the custom form would not re-read; the generic form is lossless

// GENERIC: "func.func"()
// GENERIC: "cf.switch"
// GENERIC-SAME: case_values = dense<[-2, 5]>
// The custom spelling must be gone -- it is what could not be re-read.
// GENERIC-NOT: cf.switch %
// GENERIC-NOT: 18446744073709551614

//--- plain.c
int on_int(int v) {
  switch (v) {
  case -2:
    return 1;
  case 5:
    return 2;
  default:
    return 0;
  }
}

// PLAIN: func.func @on_int
// PLAIN: cf.switch %{{[0-9]+}} : i32, [
// PLAIN-NEXT: default: ^bb{{[0-9]+}},
// PLAIN-NEXT: 4294967294: ^bb{{[0-9]+}},
// PLAIN-NEXT: 5: ^bb{{[0-9]+}}
// PLAIN-NEXT: ]
// PLAIN-NOT: "cf.switch"

// NOREMARK-NOT: remark
