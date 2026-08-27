// FR-61f-d: a range-eligible `for` body may contain an `if`/`else`, emitted as
// an `emitrust.if` REGION PAIR nested inside the single-block `emitrust.for`
// region -- not as cf blocks.
//
// This pins the IR shape, which is the whole mechanism. `createBlock`
// (ImportCFunctions.cpp) always appends to the FUNCTION region, so the cf
// lowering of `if` would emit a `cf.cond_br` inside the `emitrust.for` region
// naming blocks OUTSIDE it. `emitIfStmt` therefore has a second, structured
// arm that fires while a lifted for body is being emitted. `emitrust.if` is
// `SingleBlockImplicitTerminator<"emitrust::YieldOp">` with `results = (outs)`,
// so both arms are statement-mode regions and nothing flows out as a value --
// every scalar the arms touch is already an `emitrust.variable` place, put
// there by `collectRangeForPlaceScalars`.
//
// The frontier stays exactly where it was: `emitrust.for` HAS NO EXIT EDGE, so
// a `break`/`continue`/`return`/`goto` inside the `if` still refuses the lift
// and the whole loop falls back to the cf `while` lowering. Admitting those is
// an op-design change, not a widening. Pinned from both sides here.
//
// RUN: emitrust-import-c %s | FileCheck %s

// A plain `if` with no else: one region, one `emitrust.for`, zero cf edges.
// CHECK-LABEL: func.func @count_nonzero
// CHECK:         emitrust.for
// CHECK:           emitrust.if
// CHECK-NOT:     cf.br
int count_nonzero(int n) {
  int a[8];
  int nz = 0;
  for (int i = 0; i < 8; i++)
    a[i] = (i * n) & 1;
  for (int i = 0; i < 8; i++)
    if (a[i] != 0)
      nz++;
  return nz;
}

// `if`/`else`: BOTH regions materialize, and the printed form spells the
// second one `} else {` -- that is what proves the else arm is a region of the
// same op rather than a second `emitrust.if`.
// CHECK-LABEL: func.func @split_sum
// CHECK:         emitrust.for
// CHECK:           emitrust.if
// CHECK:           } else {
// CHECK-NOT:     cf.br
int split_sum(int n) {
  int lo = 0;
  int hi = 0;
  for (int i = 0; i < n; i++) {
    if (((i * 7) & 3) < 2)
      lo += i;
    else
      hi += i * 2;
  }
  return hi - lo;
}

// Nesting: an `if` inside an `if` inside the region.
// CHECK-LABEL: func.func @nested_if
// CHECK:         emitrust.for
// CHECK:           emitrust.if
// CHECK:             emitrust.if
// CHECK-NOT:     cf.br
int nested_if(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    if (i > 2) {
      if ((i & 1) != 0)
        s += i;
    }
  }
  return s;
}

// FRONTIER: a `break` inside the `if`. `emitrust.for` has no exit edge, so the
// loop must NOT lift -- the whole thing falls back to the cf `while` lowering.
// CHECK-LABEL: func.func @break_in_if
// CHECK-NOT:     emitrust.for
// CHECK:         cf.br
int break_in_if(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    if (i > 4)
      break;
    s += i;
  }
  return s;
}

// FRONTIER: a `return` inside the `if`, same reason.
// CHECK-LABEL: func.func @return_in_if
// CHECK-NOT:     emitrust.for
// CHECK:         cf.br
int return_in_if(int n) {
  for (int i = 0; i < n; i++) {
    if (i * i > n)
      return i;
  }
  return -1;
}

// FRONTIER: a `?:` in the body stays out this wave. `emitConditionalOperator`
// routes its result through `createEntryAlloca`, and an alloca whose loads and
// stores live inside the region fails legalization ("failed to legalize
// operation 'memref.alloca' that was explicitly marked illegal" -- spike
// 61f-0). `emitrust.if` has `results = (outs)`, so there is no
// if-EXPRESSION escape hatch; routing the value to an `emitrust.variable`
// place is a separate increment, and measured corpus demand for it is ~0.
// CHECK-LABEL: func.func @ternary_in_body
// CHECK-NOT:     emitrust.for
// CHECK:         cf.br
int ternary_in_body(int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += (i & 1) ? i : -i;
  return s;
}
