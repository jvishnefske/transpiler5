// Pins the Rust text `emitrust.neg` renders and, in particular, the
// PARENTHESIZATION of its operand. Rust's prefix `-` on f32/f64 is
// `std::ops::Neg`, i.e. IEEE-754 negation, which is the only spelling that
// agrees with C's unary minus on signed zero and NaN sign; the previous
// `0.0 - x` lowering was a miscompile there.
//
// The operand of a prefix `-` is a unary-expression, so every looser rank
// must wrap or the text re-associates: `-(a + b)` (an infix operand),
// `-(x as f64)` (a cast -- `-x as f64` parses as `(-x) as f64`), and
// `- -x` / `-(-1.5f64)` (a nested unary, which must never lex as `--`).
// A postfix atom -- a name, a call, a field chain -- stays bare.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// A plain name operand renders bare, at both widths.
// CHECK-LABEL: fn neg_name(v0: f64, v1: f32) {
// CHECK-NEXT:    sink(-v0, -v1);
emitrust.func @neg_name(%arg0: f64, %arg1: f32) {
  %0 = emitrust.neg %arg0 : f64
  %1 = emitrust.neg %arg1 : f32
  emitrust.call_opaque "sink"(%0, %1) : (f64, f32) -> ()
  emitrust.return
}

// An infix operand wraps: `-(a + b)`, not `-a + b`.
// CHECK-LABEL: fn neg_infix(v0: f64, v1: f64) {
// CHECK-NEXT:    sink(-(v0 + v1), -(v0 * v1));
emitrust.func @neg_infix(%arg0: f64, %arg1: f64) {
  %0 = emitrust.add %arg0, %arg1 : f64
  %1 = emitrust.neg %0 : f64
  %2 = emitrust.mul %arg0, %arg1 : f64
  %3 = emitrust.neg %2 : f64
  emitrust.call_opaque "sink"(%1, %3) : (f64, f64) -> ()
  emitrust.return
}

// A cast operand wraps: `-x as f64` would parse as `(-x) as f64`.
// CHECK-LABEL: fn neg_cast(v0: i32) {
// CHECK-NEXT:    sink(-(v0 as f64));
emitrust.func @neg_cast(%arg0: i32) {
  %0 = emitrust.cast %arg0 : i32 to f64
  %1 = emitrust.neg %0 : f64
  emitrust.call_opaque "sink"(%1) : (f64) -> ()
  emitrust.return
}

// A nested negation wraps, so the text can never lex as `--`.
// CHECK-LABEL: fn neg_nested(v0: f64) {
// CHECK-NEXT:    sink(-(-v0));
emitrust.func @neg_nested(%arg0: f64) {
  %0 = emitrust.neg %arg0 : f64
  %1 = emitrust.neg %0 : f64
  emitrust.call_opaque "sink"(%1) : (f64) -> ()
  emitrust.return
}

// A negative float CONSTANT operand wraps for the same reason.
// CHECK-LABEL: fn neg_constant() {
// CHECK-NEXT:    sink(-(-1.5f64));
emitrust.func @neg_constant() {
  %0 = emitrust.constant <-1.500000e+00 : f64> : f64
  %1 = emitrust.neg %0 : f64
  emitrust.call_opaque "sink"(%1) : (f64) -> ()
  emitrust.return
}

// A call result keeps its own binding (a call is not in the pure-inlining
// set), so the negation takes a plain name and stays bare.
// CHECK-LABEL: fn neg_call(v0: f64) {
// CHECK-NEXT:    let v1: f64 = f(v0);
// CHECK-NEXT:    sink(-v1);
emitrust.func @neg_call(%arg0: f64) {
  %0 = emitrust.call_opaque "f"(%arg0) : (f64) -> f64
  %1 = emitrust.neg %0 : f64
  emitrust.call_opaque "sink"(%1) : (f64) -> ()
  emitrust.return
}

// The negation itself is a unary-rank operand of its consumers: bare under
// an infix parent whose operators bind looser (`-v0 + v1`), but wrapped in
// the receiver and cast-source positions, where a bare `-` would bind the
// trailing `.to_bits()` / ` as T` to the operand instead -- `-v0.to_bits()`
// negates the CALL RESULT, a type error at best and a wrong value at worst.
// CHECK-LABEL: fn neg_as_operand(v0: f64, v1: f64) {
// CHECK-NEXT:    sink(-v0 + v1, (-v0).to_bits(), (-v0) as i32);
emitrust.func @neg_as_operand(%arg0: f64, %arg1: f64) {
  %0 = emitrust.neg %arg0 : f64
  %1 = emitrust.add %0, %arg1 : f64
  %2 = emitrust.neg %arg0 : f64
  %3 = emitrust.bitcast %2 : f64 to ui64
  %4 = emitrust.neg %arg0 : f64
  %5 = emitrust.cast %4 : f64 to i32
  emitrust.call_opaque "sink"(%1, %3, %5) : (f64, ui64, i32) -> ()
  emitrust.return
}

// A multi-use negation keeps its own `let` binding and renders as a name.
// CHECK-LABEL: fn neg_multi_use(v0: f64) {
// CHECK-NEXT:    let v1: f64 = -v0;
// CHECK-NEXT:    sink(v1, v1);
emitrust.func @neg_multi_use(%arg0: f64) {
  %0 = emitrust.neg %arg0 : f64
  emitrust.call_opaque "sink"(%0, %0) : (f64, f64) -> ()
  emitrust.return
}
