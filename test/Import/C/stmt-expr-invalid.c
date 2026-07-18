// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/void-value.c 2>&1 | FileCheck %s --check-prefix=VOIDVAL
// RUN: not emitrust-import-c %t/goto-out.c 2>&1 | FileCheck %s --check-prefix=GOTOOUT

// Statement-expression boundaries. The flattening model (see stmt-expr.c)
// covers value-producing StmtExprs whose control flow stays inside the
// expression; the shapes below would abandon or fabricate a value
// mid-expression and stay located rejections.

// A StmtExpr whose last statement is not an expression has void type;
// consuming it as a value is ill-formed C and clang itself rejects it
// before import.
// VOIDVAL: void-value.c:{{[0-9]+}}:{{[0-9]+}}: error: initializing 'int' with an expression of incompatible type 'void'

//--- void-value.c
int f(void) {
  int v = ({ ; });
  return v;
}

// A goto that leaves a value-position StmtExpr abandons the expression
// mid-evaluation: the synthesized value temp is never written and the
// consumer would read garbage, so the shape is rejected.
// GOTOOUT: goto-out.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: goto out of a statement expression in value position

//--- goto-out.c
int g(int n) {
  int v = ({ if (n) goto out; n + 1; });
  return v;
out:
  return -1;
}

// NOTE (RED amendment): a constant-condition ternary whose dead arm
// contains a goto-targeted label (the 00213 kb_wait_1 shape) is NOT
// rejected: the label is necessarily targeted only from within the arm
// (clang rejects any jump into a statement expression from outside), so
// the arm keeps FULL lowering instead of elision — pinned positively by
// 00213.c in the c-testsuite ledger.
