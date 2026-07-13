// FR: unsigned arith operations are an explicit MVP boundary; the
// conversion must fail loudly instead of emitting wrong Rust.
// RUN: not emitrust-opt --convert-arith-to-emitrust %s 2>&1 | FileCheck %s

// CHECK: failed to legalize operation 'arith.divui'
func.func @unsigned_div(%a: i32, %b: i32) -> i32 {
  %0 = arith.divui %a, %b : i32
  return %0 : i32
}

// The unsigned comparison below is equally illegal; the conversion already
// fails on the first illegal operation above.
func.func @unsigned_cmp(%a: i32, %b: i32) -> i1 {
  %0 = arith.cmpi ult, %a, %b : i32
  return %0 : i1
}
