// FR / C99-2: the unsigned-semantics arith operations never render as the
// bare Rust operator on a signless type — rendered iN, whose operators are
// SIGNED. The BINARY ones (divui/remui/shrui) are instead routed through
// the same-width uN rendering (see arith-to-emitrust.mlir); what remains
// illegal, and must fail loudly rather than silently miscompile, is every
// case with no correct Rust rendering to route through: the unsigned cmpi
// predicates on signless operands, the unsigned binary operations on a
// type with no uN counterpart, and the float predicates without an exact
// Rust operator.
// RUN: not emitrust-opt --split-input-file --convert-arith-to-emitrust %s 2>&1 \
// RUN:   | FileCheck %s

// `index` renders as Rust `usize` and has no `ui<N>` form to route the
// unsigned semantics through, so shrui on it stays illegal.
// CHECK: failed to legalize operation 'arith.shrui'
func.func @unsigned_shr_index(%a: index, %b: index) -> index {
  %0 = arith.shrui %a, %b : index
  return %0 : index
}

// -----

// Likewise for a width with no Rust integer type at all.
// CHECK: failed to legalize operation 'arith.divui'
func.func @unsigned_div_i24(%a: i24, %b: i24) -> i24 {
  %0 = arith.divui %a, %b : i24
  return %0 : i24
}

// -----

// The reverse pairing — shrsi on an unsigned type, whose Rust `>>` is the
// LOGICAL shift — is rejected one level earlier, by the Arith verifier
// itself: arith operations do not admit non-signless operand types at all.
// CHECK: 'arith.shrsi' op operand #0 must be signless-integer-like
func.func @signed_shr_on_unsigned(%a: ui32, %b: ui32) -> ui32 {
  %0 = "arith.shrsi"(%a, %b) : (ui32, ui32) -> ui32
  return %0 : ui32
}

// -----

// cmpi ult on signless i32 would render as the SIGNED `<` and stays
// illegal.
// CHECK: failed to legalize operation 'arith.cmpi'
func.func @unsigned_cmp(%a: i32, %b: i32) -> i1 {
  %0 = arith.cmpi ult, %a, %b : i32
  return %0 : i1
}

// -----

// All four unsigned predicates are guarded, not just ult.
// CHECK: failed to legalize operation 'arith.cmpi'
func.func @unsigned_cmp_uge(%a: i32, %b: i32) -> i1 {
  %0 = arith.cmpi uge, %a, %b : i32
  return %0 : i1
}

// -----

// `one` (ordered-and-unequal) is NOT Rust `!=`, which is unordered-or-
// unequal (une): the two differ on NaN inputs. Nothing in the pipeline
// produces one (the importer emits une for C `!=`), so it stays illegal.
// CHECK: failed to legalize operation 'arith.cmpf'
func.func @ordered_one_cmp(%a: f64, %b: f64) -> i1 {
  %0 = arith.cmpf one, %a, %b : f64
  return %0 : i1
}

// -----

// The remaining unordered predicates have no Rust operator equivalent.
// CHECK: failed to legalize operation 'arith.cmpf'
func.func @unordered_ult_cmp(%a: f64, %b: f64) -> i1 {
  %0 = arith.cmpf ult, %a, %b : f64
  return %0 : i1
}

// -----

// CHECK: failed to legalize operation 'arith.cmpf'
func.func @unordered_uno_cmp(%a: f64, %b: f64) -> i1 {
  %0 = arith.cmpf uno, %a, %b : f64
  return %0 : i1
}
