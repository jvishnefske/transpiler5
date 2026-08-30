// RUN: emitrust-import-c %s | FileCheck %s

// FR-155 (silent miscompile). Two independent mechanisms collided here:
//
//   1. `createVariablePlace` hoists EVERY `emitrust.variable` to the
//      function entry block when the function contains a label, so a goto
//      that jumps over a declaration cannot leave a later use undominated.
//   2. FR-61f attaches a compile-time-constant initializer to the variable
//      op ITSELF for place-backed signed scalars, so the emitter renders
//      `let mut s: i32 = 0;` rather than a late `let mut s; s = 0;`
//      (clippy::needless_late_init).
//
// When both fired, the hoist carried the INITIALIZER out of the loop along
// with the declaration: a local declared in a loop body was initialized
// once for the whole function instead of once per iteration, and the
// emitted crate silently printed a different number than the C program.
// Adding a dead `goto`/label to a function changed its answer.
//
// This file pins the IR shape that makes that impossible. In a function
// with a label the hoisted place for a loop-body local carries NO init
// attribute, and the initializing `emitrust.assign` is the FIRST op inside
// the loop region, where it runs once per iteration. The label-free
// control below keeps the FR-61f fast path, so the cosmetic
// needless_late_init win is surrendered only where correctness demands it.
// The hoist itself is still required and still happens -- the place ops
// below sit in the entry block, and test/Import/C/goto.c pins the
// jumped-over declaration that the hoist exists for.
//
// The behavioural oracle is test/EndToEnd/label-hoist-reinit.c; this file
// is what stops the fast path from being quietly reintroduced.

int reset_labelled(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    int s = 0;
    s += i;
    total += s;
  }
  if (total < 0)
    goto done;
done:
  return total;
}

// The place is hoisted (a label is present) but MUST NOT carry `<0 : i32>`;
// the reset is an assign inside the `emitrust.for` region instead.
// CHECK-LABEL: func.func @reset_labelled
// CHECK-NOT:   emitrust.variable named "s" <
// CHECK:       %[[S:.*]] = emitrust.variable named "s" : !emitrust.lvalue<i32>
// CHECK:       emitrust.for
// CHECK-NEXT:    %[[Z:.*]] = arith.constant 0 : i32
// CHECK-NEXT:    emitrust.assign %[[S]] = %[[Z]] : !emitrust.lvalue<i32>

int reset_nolabel(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    int s = 0;
    s += i;
    total += s;
  }
  return total;
}

// The control: with no label there is no hoist, the declaration stays in
// the loop body, and FR-61f's init attribute rides along with it.
// CHECK-LABEL: func.func @reset_nolabel
// CHECK:       emitrust.for
// CHECK-NEXT:    emitrust.variable named "s" <0 : i32> : !emitrust.lvalue<i32>

int nested_labelled(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < 3; j++) {
      int s = 1;
      s += i + j;
      total += s;
    }
  }
  if (total < 0)
    goto done;
done:
  return total;
}

// An INNER-loop local: a lost reset here compounds across both loops, so
// the reset must land inside the inner region, not the outer one and not
// the entry block.
// CHECK-LABEL: func.func @nested_labelled
// CHECK-NOT:   emitrust.variable named "s" <
// CHECK:       %[[NS:.*]] = emitrust.variable named "s" : !emitrust.lvalue<i32>
// CHECK:       emitrust.for
// CHECK:       emitrust.for
// CHECK-NEXT:    %[[NZ:.*]] = arith.constant 1 : i32
// CHECK-NEXT:    emitrust.assign %[[NS]] = %[[NZ]] : !emitrust.lvalue<i32>
