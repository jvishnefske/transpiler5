// FR-61d: single-use expression inlining and unused-pure-value drops. This
// test pins the two invariants the stage adds: (1) a pure single-real-use
// producer (constant, arithmetic, comparison, cast, bitcast, load, alias
// let) renders inline at its consumer -- parenthesized exactly as the
// consumer position's precedence demands, never in a position the denied
// `unused_parens` lint watches -- instead of through a `let` binding; (2) a
// pure producer with no emitted use at all emits nothing, cascading through
// chains of pure ops (a dropped consumer's operands drop too) while calls
// and other side-effecting ops are never dropped. Both mechanisms must keep
// the surviving v-numbering identical to the un-inlined rendering (folded
// and dropped ops still consume their number), so inlining can never
// renumber a neighbor, and must leave the FR-61a tail fold byte-identical.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// An expression tree collapses into one expression at the final consumer:
// the multiplication binds tighter than the inlined addition on its
// left-hand side, so the addition is parenthesized; the constants ride
// along with literal type suffixes.
// CHECK-LABEL: fn tree_fold(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    (v0 * 2i32 + v1) * 3i32
// CHECK-NEXT:  }
emitrust.func @tree_fold(%arg0: i32, %arg1: i32) -> i32 {
  %c2 = emitrust.constant <2 : i32> : i32
  %m = emitrust.mul %arg0, %c2 : i32
  %s = emitrust.add %m, %arg1 : i32
  %c3 = emitrust.constant <3 : i32> : i32
  %r = emitrust.mul %s, %c3 : i32
  emitrust.return %r : i32
}

// A subscript index is a cast source when the ` as usize` conversion is
// appended: the inlined subtraction must parenthesize or the cast would
// bind to its right operand only.
// CHECK-LABEL: fn subscript_index(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: [i32; 4] = [0; 4];
// CHECK-NEXT:    v1[(v0 - 1i32) as usize]
// CHECK-NEXT:  }
emitrust.func @subscript_index(%arg0: i32) -> i32 {
  %arr = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  %c1 = emitrust.constant <1 : i32> : i32
  %idx = emitrust.sub %arg0, %c1 : i32
  %e = emitrust.subscript %arr[%idx] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
  %v = emitrust.load %e : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %v : i32
}

// A method-call receiver binds tighter than any infix operator, so an
// inlined infix expression (an unsigned division stays infix) needs parens
// as the receiver of the wrapping-multiply method form.
// CHECK-LABEL: fn receiver_parens(v0: u32, v1: u32, v2: u32) -> u32 {
// CHECK-NEXT:    (v0 / v1).wrapping_mul(v2)
// CHECK-NEXT:  }
emitrust.func @receiver_parens(%arg0: ui32, %arg1: ui32, %arg2: ui32) -> ui32 {
  %q = emitrust.div %arg0, %arg1 : ui32
  %r = emitrust.mul %q, %arg2 : ui32
  emitrust.return %r : ui32
}

// A negative literal receiver must parenthesize: `-1.5f64.to_bits()` would
// negate the method result, not the literal.
// CHECK-LABEL: fn neg_receiver() -> u64 {
// CHECK-NEXT:    (-1.5f64).to_bits()
// CHECK-NEXT:  }
emitrust.func @neg_receiver() -> ui64 {
  %c = emitrust.constant <-1.5 : f64> : f64
  %b = emitrust.bitcast %c : f64 to ui64
  emitrust.return %b : ui64
}

// A negative literal as a cast source parenthesizes; a cast chained from
// another cast does not (`as` chains are left-associative and legal bare).
// CHECK-LABEL: fn neg_cast() -> u32 {
// CHECK-NEXT:    (-5i32) as u32
// CHECK-NEXT:  }
emitrust.func @neg_cast() -> ui32 {
  %c = emitrust.constant <-5 : i32> : i32
  %r = emitrust.cast %c : i32 to ui32
  emitrust.return %r : ui32
}

// CHECK-LABEL: fn cast_chain(v0: i32) -> u64 {
// CHECK-NEXT:    v0 as i64 as u64
// CHECK-NEXT:  }
emitrust.func @cast_chain(%arg0: i32) -> ui64 {
  %a = emitrust.cast %arg0 : i32 to i64
  %b = emitrust.cast %a : i64 to ui64
  emitrust.return %b : ui64
}

// Grammar quirk, not precedence: a cast directly LEFT of `<<` or `<` must
// parenthesize -- rustc parses `v0 as i64 << v1` as generic arguments on
// the type. The right operand needs nothing.
// CHECK-LABEL: fn cast_shift_lhs(v0: i32, v1: i64) -> i64 {
// CHECK-NEXT:    (v0 as i64) << v1
// CHECK-NEXT:  }
emitrust.func @cast_shift_lhs(%arg0: i32, %arg1: i64) -> i64 {
  %c = emitrust.cast %arg0 : i32 to i64
  %r = emitrust.shl %c, %arg1 : i64
  emitrust.return %r : i64
}

// CHECK-LABEL: fn cast_cmp_lhs(v0: i32, v1: i64) -> bool {
// CHECK-NEXT:    (v0 as i64) < v1
// CHECK-NEXT:  }
emitrust.func @cast_cmp_lhs(%arg0: i32, %arg1: i64) -> i1 {
  %c = emitrust.cast %arg0 : i32 to i64
  %r = emitrust.cmp lt, %c, %arg1 : (i64, i64) -> i1
  emitrust.return %r : i1
}

// The quirk follows the TRAILING token, not the top-level rank: a shift
// whose bare right operand is a cast ends in that cast, so as the left
// operand of a comparison the whole shift must parenthesize.
// CHECK-LABEL: fn shift_trailing_cast(v0: i32, v1: u16, v2: i32) -> bool {
// CHECK-NEXT:    (v0 << v1 as i32) < v2
// CHECK-NEXT:  }
emitrust.func @shift_trailing_cast(%arg0: i32, %arg1: ui16, %arg2: i32) -> i1 {
  %c = emitrust.cast %arg1 : ui16 to i32
  %sh = emitrust.shl %arg0, %c : i32
  %r = emitrust.cmp lt, %sh, %arg2 : (i32, i32) -> i1
  emitrust.return %r : i1
}

// A comparison inlined into a binary operand parenthesizes (comparison is
// the loosest inlined rank) ...
// CHECK-LABEL: fn cmp_parens(v0: i32, v1: i32, v2: i32, v3: i32) -> bool {
// CHECK-NEXT:    (v0 < v1) & (v2 < v3)
// CHECK-NEXT:  }
emitrust.func @cmp_parens(%arg0: i32, %arg1: i32, %arg2: i32, %arg3: i32) -> i1 {
  %p = emitrust.cmp lt, %arg0, %arg1 : (i32, i32) -> i1
  %q = emitrust.cmp lt, %arg2, %arg3 : (i32, i32) -> i1
  %r = emitrust.and %p, %q : i1
  emitrust.return %r : i1
}

// ... but a comparison inlined into an `if` condition stays bare: the
// condition position never parenthesizes (denied `unused_parens`).
// CHECK-LABEL: fn cmp_cond(v0: i32, v1: i32) {
// CHECK-NEXT:    if v0 == v1 {
// CHECK-NEXT:        side_effect();
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @cmp_cond(%arg0: i32, %arg1: i32) {
  %p = emitrust.cmp eq, %arg0, %arg1 : (i32, i32) -> i1
  emitrust.if %p {
    emitrust.call_opaque "side_effect"() : () -> ()
  }
  emitrust.return
}

// Every inlined numeric constant carries its literal type suffix, so the
// dropped `let` can never orphan an inference anchor: a u64 zero, ...
// CHECK-LABEL: fn suffix_zero(v0: u64) -> bool {
// CHECK-NEXT:    v0 == 0u64
// CHECK-NEXT:  }
emitrust.func @suffix_zero(%arg0: ui64) -> i1 {
  %z = emitrust.constant <0 : ui64> : ui64
  %r = emitrust.cmp eq, %arg0, %z : (ui64, ui64) -> i1
  emitrust.return %r : i1
}

// ... a large literal as a cast source (no parens: a literal is an atom), ...
// CHECK-LABEL: fn suffix_large_cast() -> i64 {
// CHECK-NEXT:    10000000000u64 as i64
// CHECK-NEXT:  }
emitrust.func @suffix_large_cast() -> i64 {
  %c = emitrust.constant <10000000000 : ui64> : ui64
  %r = emitrust.cast %c : ui64 to i64
  emitrust.return %r : i64
}

// ... a suffixed literal receiver (the suffix is what makes the bare method
// call parse as a call on the literal), ...
// CHECK-LABEL: fn suffix_receiver(v0: u32) -> u32 {
// CHECK-NEXT:    2u32.wrapping_add(v0)
// CHECK-NEXT:  }
emitrust.func @suffix_receiver(%arg0: ui32) -> ui32 {
  %two = emitrust.constant <2 : ui32> : ui32
  %r = emitrust.add %two, %arg0 : ui32
  emitrust.return %r : ui32
}

// ... and an exponent float literal (the suffix legally follows the
// exponent).
// CHECK-LABEL: fn suffix_exp_float(v0: f64) -> f64 {
// CHECK-NEXT:    v0 * 1e+30f64
// CHECK-NEXT:  }
emitrust.func @suffix_exp_float(%arg0: f64) -> f64 {
  %c = emitrust.constant <1.0E+30 : f64> : f64
  %r = emitrust.mul %arg0, %c : f64
  emitrust.return %r : f64
}

// An alias `let` (non-mut, never assigned) inlines as its right-hand side;
// chained through a deref-rooted load the whole chain renders `*v0`, whose
// unary rank binds tighter than the multiply so no parens appear.
// CHECK-LABEL: fn deref_load(v0: &i32) -> i32 {
// CHECK-NEXT:    *v0 * 10i32
// CHECK-NEXT:  }
emitrust.func @deref_load(%arg0: !emitrust.ref<i32>) -> i32 {
  %r = emitrust.let %arg0 : !emitrust.ref<i32>
  %d = emitrust.deref %r : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  %v = emitrust.load %d : (!emitrust.lvalue<i32>) -> i32
  %c = emitrust.constant <10 : i32> : i32
  %s = emitrust.mul %v, %c : i32
  emitrust.return %s : i32
}

// NOT inlined: a multi-use NON-CONSTANT keeps its binding (multi-use
// CONSTANTS duplicate instead -- see dup_short below). The binding keeps
// its baseline number: the captured single-use constant consumed v1.
// CHECK-LABEL: fn multi_use(v0: i32) -> i32 {
// CHECK-NEXT:    let v2: i32 = v0 + 7i32;
// CHECK-NEXT:    v2 * v2
// CHECK-NEXT:  }
emitrust.func @multi_use(%arg0: i32) -> i32 {
  %c = emitrust.constant <7 : i32> : i32
  %a = emitrust.add %arg0, %c : i32
  %b = emitrust.mul %a, %a : i32
  emitrust.return %b : i32
}

// NOT inlined: a STATE-READING producer (a load) with its use in a nested
// block -- the same-block rule only binds producers whose render point
// matters; constants are exempt (slice 2), so the load pins it here.
// CHECK-LABEL: fn cross_block(v0: bool, v1: i32) -> i32 {
// CHECK-NEXT:    let v2: i32 = 5;
// CHECK-NEXT:    let v3: i32 = v2;
// CHECK-NEXT:    let mut v4: i32 = v1;
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:        v4 = v3;
// CHECK-NEXT:    }
// CHECK-NEXT:    v4
// CHECK-NEXT:  }
emitrust.func @cross_block(%arg0: i1, %arg1: i32) -> i32 {
  %v = emitrust.variable <5 : i32> : !emitrust.lvalue<i32>
  %c = emitrust.load %v : (!emitrust.lvalue<i32>) -> i32
  %m = emitrust.let mut %arg1 : i32
  emitrust.if %arg0 {
    emitrust.assign %m = %c : i32
  }
  emitrust.return %m : i32
}

// A single-use constant DOES cross into a nested block (slice 2 relaxed
// the same-block rule for position-independent literals).
// CHECK-LABEL: fn cross_block_const(v0: bool, v1: i32) -> i32 {
// CHECK-NEXT:    let mut v3: i32 = v1;
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:        v3 = 5i32;
// CHECK-NEXT:    }
// CHECK-NEXT:    v3
// CHECK-NEXT:  }
emitrust.func @cross_block_const(%arg0: i1, %arg1: i32) -> i32 {
  %c = emitrust.constant <5 : i32> : i32
  %m = emitrust.let mut %arg1 : i32
  emitrust.if %arg0 {
    emitrust.assign %m = %c : i32
  }
  emitrust.return %m : i32
}

// NOT inlined: a call between a STATE-READING def (a load) and its use is
// a barrier (the call may mutate what the moved text would re-read;
// unknown ops block by default). Constants are exempt from the barrier
// rule since slice 2 -- a literal reads the same everywhere.
// CHECK-LABEL: fn call_barrier(_v0: i32) -> i32 {
// CHECK-NEXT:    let v1: i32 = 9;
// CHECK-NEXT:    let v2: i32 = v1;
// CHECK-NEXT:    let v3: i32 = get();
// CHECK-NEXT:    v2 + v3
// CHECK-NEXT:  }
emitrust.func @call_barrier(%arg0: i32) -> i32 {
  %v = emitrust.variable <9 : i32> : !emitrust.lvalue<i32>
  %c = emitrust.load %v : (!emitrust.lvalue<i32>) -> i32
  %g = emitrust.call_opaque "get"() : () -> i32
  %r = emitrust.add %c, %g : i32
  emitrust.return %r : i32
}

// FR-61f: `emitrust.for` bounds/step render through `emitOperand`, so the
// constant lower bound inlines into the range head and the unit step drops
// `.step_by` -- `for i in 0..n` instead of `let v1=0; let v2=1; (v1..v0)
// .step_by(v2 as usize)`.
// CHECK-LABEL: fn for_bound(v0: usize) {
// CHECK-NEXT:    for _v3 in 0usize..v0 {
// CHECK-NEXT:        body();
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @for_bound(%arg0: index) {
  %zero = emitrust.constant <0 : index> : index
  %one = emitrust.constant <1 : index> : index
  emitrust.for %i = %zero to %arg0 step %one {
    emitrust.call_opaque "body"() : () -> ()
  }
  emitrust.return
}

// NOT inlined: a literal producer (its text is opaque; only the closed pure
// set inlines).
// CHECK-LABEL: fn literal_producer(v0: usize) -> usize {
// CHECK-NEXT:    let v1: usize = 10;
// CHECK-NEXT:    v0 + v1
// CHECK-NEXT:  }
emitrust.func @literal_producer(%arg0: index) -> index {
  %l = emitrust.literal "10" : index
  %r = emitrust.add %arg0, %l : index
  emitrust.return %r : index
}

// NOT inlined: a select producer (renders as a statement-level if-else
// expression; folding it into an operand is out of scope).
// CHECK-LABEL: fn select_producer(v0: bool, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let v3: i32 = if v0 { v1 } else { v2 };
// CHECK-NEXT:    v3 + 1i32
// CHECK-NEXT:  }
emitrust.func @select_producer(%arg0: i1, %arg1: i32, %arg2: i32) -> i32 {
  %s = emitrust.select %arg0, %arg1, %arg2 : i32
  %c = emitrust.constant <1 : i32> : i32
  %r = emitrust.add %s, %c : i32
  emitrust.return %r : i32
}

// DROP: an unused pure value emits nothing at all -- and the drop cascades
// to the operands it was the only reader of.
// CHECK-LABEL: fn drop_unused(v0: i32) -> i32 {
// CHECK-NEXT:    v0
// CHECK-NEXT:  }
emitrust.func @drop_unused(%arg0: i32) -> i32 {
  %c = emitrust.constant <3 : i32> : i32
  %u = emitrust.add %arg0, %c : i32
  emitrust.return %arg0 : i32
}

// DROP: the cascade runs through loads and projections: dropping the unused
// add drops the element load, which un-reads the array (now `_`-prefixed)
// and the index parameter.
// CHECK-LABEL: fn drop_cascade(_v0: i32) {
// CHECK-NEXT:    let _v1: [i32; 4] = [0; 4];
// CHECK-NEXT:  }
emitrust.func @drop_cascade(%arg0: i32) {
  %arr = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  %e = emitrust.subscript %arr[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
  %v = emitrust.load %e : (!emitrust.lvalue<i32>) -> i32
  %one = emitrust.constant <1 : i32> : i32
  %s = emitrust.add %v, %one : i32
  emitrust.return
}

// DROP: an unused division still drops. The dropped `let` removes a
// divide-by-zero panic that exists only where the C program divided by
// zero -- C UB, so eliding it refines UB exactly like the existing
// dead-store elision.
// CHECK-LABEL: fn drop_div(v0: i32, _v1: i32) -> i32 {
// CHECK-NEXT:    v0
// CHECK-NEXT:  }
emitrust.func @drop_div(%arg0: i32, %arg1: i32) -> i32 {
  %q = emitrust.div %arg0, %arg1 : i32
  emitrust.return %arg0 : i32
}

// NOT dropped: a call's result may be unused but the call itself is
// observable; it keeps its (underscore-named) binding.
// CHECK-LABEL: fn keep_call(v0: i32) -> i32 {
// CHECK-NEXT:    let _v1: i32 = observable();
// CHECK-NEXT:    v0
// CHECK-NEXT:  }
emitrust.func @keep_call(%arg0: i32) -> i32 {
  %r = emitrust.call_opaque "observable"() : () -> i32
  emitrust.return %arg0 : i32
}

// FR-61a interplay: the tail fold renders exactly as before (the candidate
// is excluded from inlining, so the two mechanisms never double-handle).
// CHECK-LABEL: fn tail_fold_kept(v0: i32) -> i32 {
// CHECK-NEXT:    v0 + v0
// CHECK-NEXT:  }
emitrust.func @tail_fold_kept(%arg0: i32) -> i32 {
  %0 = emitrust.add %arg0, %arg0 : i32
  emitrust.return %0 : i32
}

// Numbering stability: the inlined constant still consumed v1, so the
// surviving binding keeps its baseline number v2 (a gap, never a renumber).
// CHECK-LABEL: fn numbering_gap(v0: i32) -> i32 {
// CHECK-NEXT:    let v2: i32 = v0 + 4i32;
// CHECK-NEXT:    v2 * v2
// CHECK-NEXT:  }
emitrust.func @numbering_gap(%arg0: i32) -> i32 {
  %c = emitrust.constant <4 : i32> : i32
  %a = emitrust.add %arg0, %c : i32
  %b = emitrust.mul %a, %a : i32
  emitrust.return %b : i32
}

// --- FR-61d slice 2: multi-use constant duplication ---

// A short constant (suffixed text within the measured threshold) with
// several classified uses duplicates its literal at EVERY use and the
// binding vanishes; each use parenthesizes independently.
// CHECK-LABEL: fn dup_short(v0: i32) -> i32 {
// CHECK-NEXT:    (v0 + 7i32) * 7i32 ^ 7i32
// CHECK-NEXT:  }
emitrust.func @dup_short(%arg0: i32) -> i32 {
  %c = emitrust.constant <7 : i32> : i32
  %a = emitrust.add %arg0, %c : i32
  %b = emitrust.mul %a, %c : i32
  %d = emitrust.xor %b, %c : i32
  emitrust.return %d : i32
}

// A LONG literal (suffixed text over the threshold) repeated at several
// sites reads worse than a name, so it keeps its multi-use binding.
// CHECK-LABEL: fn dup_long(v0: u64) -> u64 {
// CHECK-NEXT:    let v1: u64 = 10000000000;
// CHECK-NEXT:    v0.wrapping_add(v1).wrapping_mul(v1)
// CHECK-NEXT:  }
emitrust.func @dup_long(%arg0: ui64) -> ui64 {
  %c = emitrust.constant <10000000000 : ui64> : ui64
  %a = emitrust.add %arg0, %c : ui64
  %b = emitrust.mul %a, %c : ui64
  emitrust.return %b : ui64
}

// A duplicated negative constant parenthesizes per use: bare as a binary
// operand, wrapped as a method receiver.
// CHECK-LABEL: fn dup_negative(v0: f64) -> u64 {
// CHECK-NEXT:    (v0 * -1.5f64).to_bits().wrapping_add((-1.5f64).to_bits())
// CHECK-NEXT:  }
emitrust.func @dup_negative(%arg0: f64) -> ui64 {
  %c = emitrust.constant <-1.5 : f64> : f64
  %m = emitrust.mul %arg0, %c : f64
  %mb = emitrust.bitcast %m : f64 to ui64
  %cb = emitrust.bitcast %c : f64 to ui64
  %r = emitrust.add %mb, %cb : ui64
  emitrust.return %r : ui64
}

// FR-61f: the constant `1` is now a classified consumer at the for bound
// (like every other use), so as a short multi-use literal it duplicates into
// every site -- the lower bound, and the trailing `v0 + 1` -- while the unit
// step drops `.step_by`. No `let` binding survives.
// CHECK-LABEL: fn dup_for_mixed(v0: usize) -> usize {
// CHECK-NEXT:    for _v2 in 1usize..v0 {
// CHECK-NEXT:        body();
// CHECK-NEXT:    }
// CHECK-NEXT:    v0 + 1usize
// CHECK-NEXT:  }
emitrust.func @dup_for_mixed(%arg0: index) -> index {
  %one = emitrust.constant <1 : index> : index
  emitrust.for %i = %one to %arg0 step %one {
    emitrust.call_opaque "body"() : () -> ()
  }
  %r = emitrust.add %arg0, %one : index
  emitrust.return %r : index
}

// FR-61f: a NON-unit step keeps `.step_by` (the `as usize` cast is
// load-bearing for an i32 step), with the constant bound and step inlined.
// CHECK-LABEL: fn for_step2(v0: i32) {
// CHECK-NEXT:    for _v3 in (0i32..v0).step_by(2i32 as usize) {
// CHECK-NEXT:        body();
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @for_step2(%arg0: i32) {
  %zero = emitrust.constant <0 : i32> : i32
  %two = emitrust.constant <2 : i32> : i32
  emitrust.for %i = %zero to %arg0 step %two : i32 {
    emitrust.call_opaque "body"() : () -> ()
  }
  emitrust.return
}

// --- FR-61d slice 2: single-use global-load promotion ---

emitrust.global @g_counter <0 : i32> : i32

// A single-use load of a mutable global inlines like any other pure read.
// CHECK-LABEL: fn global_inline(v0: i32) -> i32 {
// CHECK-NEXT:    g_counter.with(|__emitrust_tl| __emitrust_tl.get()) + v0
// CHECK-NEXT:  }
emitrust.func @global_inline(%arg0: i32) -> i32 {
  %g = emitrust.global_load @g_counter : i32
  %r = emitrust.add %g, %arg0 : i32
  emitrust.return %r : i32
}

// An intervening store to ANY global is a barrier: moving the read's text
// past it would read the new value. The load keeps its binding.
// CHECK-LABEL: fn global_store_blocks(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: i32 = g_counter.with(|__emitrust_tl| __emitrust_tl.get());
// CHECK-NEXT:    g_counter.with(|__emitrust_tl| __emitrust_tl.set(v0));
// CHECK-NEXT:    v1 + v0
// CHECK-NEXT:  }
emitrust.func @global_store_blocks(%arg0: i32) -> i32 {
  %g = emitrust.global_load @g_counter : i32
  emitrust.global_store %arg0, @g_counter : i32
  %r = emitrust.add %g, %arg0 : i32
  emitrust.return %r : i32
}

// --- FR-61d slice 3: capture routing + if-expression tail fold ---

// The full FR-61 target shape: the deferred binding + both-arms-assign if
// whose only read is the function-final return folds into the tail
// if-expression; arm-local candidates inline (the subtraction into the
// call argument, the addition into the arm tail), and the call-produced
// binding correctly SURVIVES (calls are never inline producers).
// CHECK-LABEL: fn sum_to_shape(n: i32) -> i32 {
// CHECK-NEXT:    if n <= 0i32 {
// CHECK-NEXT:        0i32
// CHECK-NEXT:    } else {
// CHECK-NEXT:        let v5: i32 = sum_to_shape(n - 1i32);
// CHECK-NEXT:        n + v5
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @sum_to_shape(%arg0: i32) -> i32
    attributes {emitrust.param_names = ["n"]} {
  %zero = emitrust.constant <0 : i32> : i32
  %one = emitrust.constant <1 : i32> : i32
  %cond = emitrust.cmp le, %arg0, %zero : (i32, i32) -> i1
  %r = emitrust.let mut %zero : i32
  emitrust.if %cond {
    emitrust.assign %r = %zero : i32
  } else {
    %sub = emitrust.sub %arg0, %one : i32
    %call = emitrust.call_opaque "sum_to_shape"(%sub) : (i32) -> i32
    %add = emitrust.add %arg0, %call : i32
    emitrust.assign %r = %add : i32
  }
  emitrust.return %r : i32
}

// A for body now routes through the capture mechanism: body-local
// candidates inline; the induction variable's naming is untouched.
// CHECK-LABEL: fn for_body_inline(v0: usize, v1: usize, v2: usize) {
// CHECK-NEXT:    for v3 in (v0..v1).step_by(v2 as usize) {
// CHECK-NEXT:        sink(v3 * 2usize);
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @for_body_inline(%arg0: index, %arg1: index, %arg2: index) {
  emitrust.for %i = %arg0 to %arg1 step %arg2 {
    %two = emitrust.constant <2 : index> : index
    %m = emitrust.mul %i, %two : index
    emitrust.call_opaque "sink"(%m) : (index) -> ()
  }
  emitrust.return
}

// An inlined arm tail sits in the never-parens Stmt position whatever its
// rank -- here the loosest one (a comparison), bare.
// CHECK-LABEL: fn arm_tail_bare(v0: bool, v1: i32, v2: i32) -> bool {
// CHECK-NEXT:    if v0 {
// CHECK-NEXT:        v1 < v2
// CHECK-NEXT:    } else {
// CHECK-NEXT:        false
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @arm_tail_bare(%arg0: i1, %arg1: i32, %arg2: i32) -> i1 {
  %f = emitrust.constant <false> : i1
  %r = emitrust.let mut %f : i1
  emitrust.if %arg0 {
    %c = emitrust.cmp lt, %arg1, %arg2 : (i32, i32) -> i1
    emitrust.assign %r = %c : i1
  } else {
    %c2 = emitrust.constant <false> : i1
    emitrust.assign %r = %c2 : i1
  }
  emitrust.return %r : i1
}

// NOT folded: an if-expression binding with a second read keeps its `let`
// (the mul reads it twice; the mul itself tail-folds as before).
// CHECK-LABEL: fn if_expr_second_use(v0: bool, v1: i32, v2: i32) -> i32 {
// CHECK-NEXT:    let v3: i32 = if v0 {
// CHECK-NEXT:        v1
// CHECK-NEXT:    } else {
// CHECK-NEXT:        v2
// CHECK-NEXT:    };
// CHECK-NEXT:    v3 * v3
// CHECK-NEXT:  }
emitrust.func @if_expr_second_use(%arg0: i1, %arg1: i32, %arg2: i32) -> i32 {
  %r = emitrust.let mut %arg1 : i32
  emitrust.if %arg0 {
    emitrust.assign %r = %arg1 : i32
  } else {
    emitrust.assign %r = %arg2 : i32
  }
  %d = emitrust.mul %r, %r : i32
  emitrust.return %d : i32
}
