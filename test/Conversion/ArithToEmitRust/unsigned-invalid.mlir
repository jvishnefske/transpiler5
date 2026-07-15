// FR / C99-2: the unsigned-semantics arith operations only convert on
// unsigned IntegerType operands, whose Rust rendering uN carries the
// unsigned operator semantics. On signless types — rendered iN, whose
// operators are SIGNED — they must stay illegal and fail loudly instead of
// silently miscompiling. The float predicates without an exact Rust
// operator are an explicit MVP boundary for the same reason.
// RUN: not emitrust-opt --split-input-file --convert-arith-to-emitrust %s 2>&1 \
// RUN:   | FileCheck %s

// divui on signless i32 would render as the signed `/` and stays illegal.
// CHECK: failed to legalize operation 'arith.divui'
func.func @unsigned_div(%a: i32, %b: i32) -> i32 {
  %0 = arith.divui %a, %b : i32
  return %0 : i32
}

// -----

// remui on signless i32 would render as the signed `%` and stays illegal.
// CHECK: failed to legalize operation 'arith.remui'
func.func @unsigned_rem(%a: i32, %b: i32) -> i32 {
  %0 = arith.remui %a, %b : i32
  return %0 : i32
}

// -----

// shrui on signless i32 would render as the ARITHMETIC `>>` (Rust's shift
// on iN sign-extends) and stays illegal.
// CHECK: failed to legalize operation 'arith.shrui'
func.func @unsigned_shr(%a: i32, %b: i32) -> i32 {
  %0 = arith.shrui %a, %b : i32
  return %0 : i32
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
