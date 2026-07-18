// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

// GNU statement expressions `({ ... })`. The body lowers as FLATTENED
// statements in the enclosing function — never as a walled-off region op —
// so labels inside a statement expression register with the same
// labelBlocks/goto dispatch machinery as any other label. The value of the
// StmtExpr is the last expression-statement's value, landed in a
// synthesized temp that the surrounding expression reads.
//
// Constant-condition ternaries elide the dead arm BEFORE lowering (after a
// live-label check; see stmt-expr-invalid.c for the goto-target edge), so
// a dead arm may contain constructs that would otherwise be rejected.

// This function is never lowered: calls to it must be elided along with
// the dead arms that contain them.
extern int side_effect(int);

int init_value(int x) {
  int a = ({ int t = x + 1; t * 2; });
  return a;
}

// The body statements run inline; the last expression's value transits a
// temp cell that the initializer reads. No region op is created.
// CHECK-LABEL: func.func @init_value
// CHECK-NOT: scf.execute_region
// CHECK: arith.addi
// CHECK: arith.muli
// CHECK: memref.store
// CHECK: memref.load
// CHECK: return

int in_arith(int x) {
  int b = ({ x + 2; }) * 3 + ({ int u = x - 1; u; });
  return b;
}

// CHECK-LABEL: func.func @in_arith
// CHECK-NOT: scf.execute_region
// CHECK: arith.muli
// CHECK: return

int nested(int x) {
  int c = ({
    int u = ({ x + 1; });
    int v = ({ int w = u * 2; w + 3; });
    u + v;
  });
  return c;
}

// Nested statement expressions flatten recursively; each level's value
// lands in its own temp.
// CHECK-LABEL: func.func @nested
// CHECK-NOT: scf.execute_region
// CHECK: arith.addi
// CHECK: arith.muli
// CHECK: return

int const_ternary_live(int x) {
  // The condition is a compile-time constant: the dead arm (whose call has
  // no import and could never lower) is elided before lowering, and only
  // the live StmtExpr arm's code is emitted.
  int r = 1 ? ({ int t = x + 3; t * 2; }) : side_effect(x);
  return r;
}

// CHECK-LABEL: func.func @const_ternary_live
// CHECK-NOT: side_effect
// CHECK: arith.addi
// CHECK: arith.muli
// CHECK: return

int const_ternary_dead_stmtexpr(int x) {
  // The dead arm is itself a StmtExpr containing an unimportable call; it
  // is elided wholesale, and only the live plain arm lowers.
  int r = 0 ? ({ side_effect(x); }) : x + 9;
  return r;
}

// CHECK-LABEL: func.func @const_ternary_dead_stmtexpr
// CHECK-NOT: side_effect
// CHECK: arith.addi
// CHECK: return

#define STEP(v) do { (v) = (v) + ({ int _d = (v) / 2; _d + 1; }); } while (0)

int macro_step(int x) {
  STEP(x);
  STEP(x);
  return x;
}

// The classic do { ... } while (0) macro wrapper around a StmtExpr: two
// expansions, each with its own flattened body.
// CHECK-LABEL: func.func @macro_step
// CHECK-NOT: scf.execute_region
// CHECK: arith.divsi
// CHECK: return

int label_in_stmtexpr(int n) {
  int r = ({
    int acc = 0;
  again:
    acc = acc + n;
    n = n - 1;
    if (n > 0)
      goto again;
    acc;
  });
  return r;
}

// A label inside a StmtExpr registers with the ordinary labelBlocks
// dispatch: the backward goto is a plain cf.br to the label block (this is
// the 00213 within-StmtExpr shape). It must lower, not reject, and it
// lifts to a structured loop.
// CHECK-LABEL: func.func @label_in_stmtexpr
// CHECK: cf.br ^[[AGAIN:bb[0-9]+]]
// CHECK: ^[[AGAIN]]:
// CHECK: cf.cond_br
// SCF-LABEL: func.func @label_in_stmtexpr
// SCF: scf.while
// SCF-NOT: cf.br
// SCF: return

int dead_if_live_label(int i) {
  // A constant-false if whose arm holds a goto-targeted label must NOT be
  // silently elided: the arm's code stays reachable through the label and
  // keeps lowering fully (this compiles today; elision must not regress
  // it).
  if (0) {
  lab:
    i = i * 77;
    return i;
  }
  if (i > 2)
    goto lab;
  return i;
}

// CHECK-LABEL: func.func @dead_if_live_label
// CHECK: arith.constant 77
// CHECK: return
// SCF-LABEL: func.func @dead_if_live_label
// SCF-NOT: cf.br
// SCF: return

int discarded(int ret) {
  // A StmtExpr in statement position (the 00214 `bla` shape): its value is
  // discarded, and the constant-false if inside it (no labels) is elided —
  // including the declaration in the dead arm.
  ({
    if (0) {
      if (ret)
        ret = 99;
      int x = ret + 1;
    }
    ret;
  });
  return ret;
}

// CHECK-LABEL: func.func @discarded
// CHECK-NOT: arith.constant 99
// CHECK: return
