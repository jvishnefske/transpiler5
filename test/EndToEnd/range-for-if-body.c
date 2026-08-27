// FR-61f-d: a range-eligible `for` body may contain STRUCTURED control flow --
// `if`/`else` -- emitted as an `emitrust.if` inside the single-block
// `emitrust.for` region instead of cf blocks.
//
// `blocksRangeForLift` used to fence `clang::IfStmt` outright, with the same
// stale reason FR-61f-6 corrected for aggregates: "emitting this creates cf
// basic blocks". That is true of `emitIfStmt`'s ONLY arm at the time, because
// `createBlock` always appends to the FUNCTION region, so a `cf.cond_br` inside
// an `emitrust.for` region would name blocks outside it. It is not true of the
// construct. `emitrust.if` already existed, the EmitRust dialect is wholesale
// legal in `convert-to-emitrust`, and `TranslateToRust` already renders an `if`
// nested in a `for`. So this is a second emission arm for `emitIfStmt`, gated
// on `liftedForDepth > 0`, and no new op, dialect change or conversion pattern.
//
// It needs no new place machinery either: `collectRangeForPlaceScalars` already
// routes every automatic-storage integer scalar the body names to an
// `emitrust.variable` place, so a variable read or written inside the `if`
// region is a plain `emitrust.load`/`emitrust.assign` pair -- no
// `memref.alloca`, no mem2reg dependence. That is exactly why the `if` leg is
// cheap and the `?:` leg (which routes through `createEntryAlloca`) is not.
//
// MEASURED. Methodology, so the next increment's deltas are comparable:
// `emitrust-cc --emit=crate` per PROJECT, each project's input set taken from
// its own lit RUN line (paper/data/runline.py), counting `^ *for \w+ in `,
// `^ *while `, `^ *loop \{` and total lines over every emitted .rs;
// "corpus-intrinsic" is the EndToEnd set with the FR-61f family's own tests
// excluded, so the FR cannot score itself.
//   EndToEnd, corpus-intrinsic: lines 21849 -> 21749 (-100), `for .. in`
//   193 -> 200 (+7), `while` 103 -> 100 (-3), `loop {` 143 -> 139 (-4).
//   c-testsuite (220 files): lines 18107 -> 18104, `for .. in` 24 -> 25,
//   `while` 26 -> 25, `loop {` flat at 72.
//   ZERO new rejections on either corpus (the same 6 EndToEnd projects fail
//   before and after; c-testsuite 0). Six units change at all.
// Unlike FR-61f-6 all THREE loop forms move the right way -- an `if` in the
// body drags no new induction into `placeBackedScalars`, so there is no
// enclosing-loop degradation. test/EndToEnd/struct-long-arrays.c goes from 8
// `loop {` to 4 (258 -> 165 lines); the remaining 4 are held by clause 6
// (`inductionDeadAfter`), not by this clause. The FR's standing lesson held
// again: the clause's 82 measured first-failure rejections realized as +10
// emitted `for` heads -- a first-failure count is an UPPER BOUND, never a yield.
//
// `break`/`continue`/`goto`/`return`/labels/`case` STAY FENCED regardless, and
// so do `?:`/`&&`/`||`/`switch`/`va_arg`/StmtExpr/`throw`/`try` and a nested
// `for` that does not itself lift: `emitrust.for` has NO EXIT EDGE, so
// admitting a jump is an op-design change rather than a widening. Each is
// pinned below from the other side.
//
// Every case is byte-diffed against the clang-built native at several argument
// values, so nothing constant-folds and a lift that dropped, reordered or
// mis-guarded a value shows up as a wrong number rather than a compile error.
// `cargo build` success cannot see a miscompile.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/range_for_if_body > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/range_for_if_body a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/range_for_if_body a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

// A GLOBAL written inside the `if` is the one case whose pin cannot live
// beside its function: the emitter groups global-touching functions into an
// actor impl and emits that impl FIRST, ahead of every free function, so the
// pin goes here in emitted order. `emitrust.global_store` inside an
// `emitrust.if` inside an `emitrust.for` region -- and the store must still be
// observable after the loop, which is what the byte-diff proves, since a
// dropped store compiles perfectly cleanly.
// CHECK-LABEL: fn global_in_if
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-NOT:     while

int printf(const char *, ...);

int g_hits = 0;

// A plain `if` with no else -- the shape that still held the last `loop {`
// degradations in struct-long-arrays.c.
// CHECK-LABEL: fn count_nonzero
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-NOT:     while
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

// `if`/`else`: both arms are regions of the SAME op, and both write places
// that outlive the loop.
// CHECK-LABEL: fn split_sum
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-NOT:     while
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

// Nested `if` inside `if`.
// CHECK-LABEL: fn nested_if
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-NOT:     while
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

// An else-if CHAIN: the else arm holds another whole `if`, so the regions
// nest to the chain's depth.
// CHECK-LABEL: fn else_if_chain
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-NOT:     while
int else_if_chain(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    if (i % 3 == 0)
      s += 1;
    else if (i % 3 == 1)
      s += 10;
    else
      s += 100;
  }
  return s;
}

// A DECLARATION inside an arm: the arm is its own Rust block, so the `let`
// scopes to it and shadows nothing outside.
// CHECK-LABEL: fn decl_in_arm
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-NOT:     while
int decl_in_arm(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    if ((i & 1) != 0) {
      int t = i * 3 + 1;
      s += t;
    } else {
      int t = i * 5;
      s -= t;
    }
  }
  return s;
}

// A LIFTED `for` nested inside the `if`: an `emitrust.for` region inside an
// `emitrust.if` region inside an `emitrust.for` region.
// CHECK-LABEL: fn for_in_if
// CHECK:         for {{i|_i}} in 0i32..
// CHECK:         for {{j|_j}} in 0i32..
// CHECK-NOT:     while
int for_in_if(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    if ((i & 1) == 0) {
      for (int j = 0; j < 4; j++)
        s += i + j;
    }
  }
  return s;
}

// An `if` inside a NESTED lifted `for` -- this is the exact shape
// range-for-aggregates.c's `nested_inner_blocked` pinned as unliftable before
// this FR. `liftedForDepth` must NEST (a counter, not a flag) for it.
// CHECK-LABEL: fn if_in_nested_for
// CHECK:         for {{i|_i}} in 0i32..
// CHECK:         for {{j|_j}} in 0i32..
// CHECK-NOT:     while
int if_in_nested_for(int n) {
  int g[4][6];
  int s = 0;
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 6; j++) {
      if (((i + j + n) & 1) != 0)
        s += i * 6 + j;
    }
  }
  (void)g;
  return s;
}

// A GLOBAL written inside the `if`. Pinned at the top of the file, in the
// actor impl's emission order -- see the block above `int printf`.
int global_in_if(int n) {
  for (int i = 0; i < n; i++)
    if ((i & 3) == 1)
      g_hits += i;
  return g_hits;
}

// A CALL with a side effect inside the `if`: ordering and count are both
// observable in the diffed stdout.
// CHECK-LABEL: fn printf_in_if
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-NOT:     while
int printf_in_if(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    if ((i % 4) == 3) {
      printf("hit %d\n", i);
      s += i;
    }
  }
  return s;
}

// FRONTIER: `break` inside the `if`. `emitrust.for` has no exit edge.
// CHECK-LABEL: fn break_in_if
// CHECK-NOT:     for {{.*}} in
// CHECK:         loop {
int break_in_if(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    if (i > 4)
      break;
    s += i;
  }
  return s;
}

// FRONTIER: `continue` inside the `if`.
// CHECK-LABEL: fn continue_in_if
// CHECK-NOT:     for {{.*}} in
// CHECK:         while
int continue_in_if(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    if ((i & 1) != 0)
      continue;
    s += i;
  }
  return s;
}

// FRONTIER: `return` inside the `if`.
// CHECK-LABEL: fn return_in_if
// CHECK-NOT:     for {{.*}} in
// CHECK:         loop {
int return_in_if(int n) {
  for (int i = 0; i < n; i++) {
    if (i * i > n)
      return i;
  }
  return -1;
}

// FRONTIER: `goto` out of the `if`.
// CHECK-LABEL: fn goto_in_if
// CHECK-NOT:     for {{.*}} in
// CHECK:         loop {
int goto_in_if(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    if (i == 5)
      goto done;
    s += i;
  }
done:
  return s;
}

// FRONTIER: a `switch` in the body -- structured emission covers `if` only
// this wave; `emitSwitchStmt` still builds cf blocks and a `case` label is a
// jump target.
// CHECK-LABEL: fn switch_in_body
// CHECK-NOT:     for {{.*}} in
// CHECK:         while
int switch_in_body(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    switch (i & 3) {
    case 0:
      s += 1;
      break;
    default:
      s += 2;
      break;
    }
  }
  return s;
}

// FRONTIER: `?:` in the body. `emitConditionalOperator` routes its result
// through `createEntryAlloca`, and an alloca whose loads/stores live inside
// the region fails legalization ("failed to legalize operation
// 'memref.alloca' that was explicitly marked illegal", spike 61f-0).
// `emitrust.if` has `results = (outs)`, so there is no if-EXPRESSION escape
// hatch. Measured corpus demand once `if` is admitted: 4 first-failures
// corpus-wide, ~0-1 realized loops. Not this wave.
// CHECK-LABEL: fn ternary_in_body
// CHECK-NOT:     for {{.*}} in
// CHECK:         while
int ternary_in_body(int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += (i & 1) ? i : -i;
  return s;
}

// FRONTIER: `&&` in the body. `emitShortCircuit` routes its i1 flag through
// `createEntryAlloca` unconditionally, same blocker. Measured corpus demand
// once `if` is admitted: EXACTLY ZERO.
// CHECK-LABEL: fn logical_and_in_body
// CHECK-NOT:     for {{.*}} in
// CHECK:         while
int logical_and_in_body(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    int hit = i > 1 && i < 6;
    s += hit;
  }
  return s;
}

int main(int argc, char **) {
  int n = argc + 6;
  printf("%d\n", count_nonzero(n));
  printf("%d\n", split_sum(n));
  printf("%d\n", nested_if(n));
  printf("%d\n", else_if_chain(n));
  printf("%d\n", decl_in_arm(n));
  printf("%d\n", for_in_if(n));
  printf("%d\n", if_in_nested_for(n));
  printf("%d\n", global_in_if(n));
  printf("%d\n", printf_in_if(n));
  printf("%d\n", break_in_if(n));
  printf("%d\n", continue_in_if(n));
  printf("%d\n", return_in_if(n));
  printf("%d\n", goto_in_if(n));
  printf("%d\n", switch_in_body(n));
  printf("%d\n", ternary_in_body(n));
  printf("%d\n", logical_and_in_body(n));
  return 0;
}
