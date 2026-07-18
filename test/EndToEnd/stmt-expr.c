// REQUIRES: cargo
// Differential test for the StmtExpr pack's expression shapes: macro-style
// min/max built from GNU statement expressions, a do { } while (0) macro
// wrapper whose body computes through a StmtExpr, nested StmtExprs
// (CLAMP expands MAXV inside its own StmtExpr), a StmtExpr in loop-carried
// arithmetic with data-dependent inputs, a label inside a StmtExpr
// targeted by a goto within the same StmtExpr, and a constant-condition
// ternary whose dead arm never executes natively and is elided by the
// importer. Byte-identical stdout and exit codes against the clang-built
// native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name stmt_expr_e2e --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stmt_expr_e2e > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern int printf(const char *, ...);

#define SWAP_ADD(a, b) do { int _t = (a); (a) = (b); (b) = _t + 1; } while (0)
#define MAXV(a, b) ({ int _a = (a); int _b = (b); _a > _b ? _a : _b; })
#define MINV(a, b) ({ int _p = (a); int _q = (b); _p < _q ? _p : _q; })
#define CLAMP(x, lo, hi) ({ int _x = (x); MAXV(MINV(_x, (hi)), (lo)); })
#define STEP(v) do { (v) = (v) + ({ int _d = (v) / 2; _d + 1; }); } while (0)

static int accum(int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += MAXV(i * 3 % 7, i);
  return s;
}

static int label_loop(int n) {
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

int main(void) {
  int x = 4, y = 9;
  SWAP_ADD(x, y);
  printf("x=%d y=%d\n", x, y);
  printf("max=%d min=%d\n", MAXV(x + 1, y - 2), MINV(x - y, y));
  printf("clamp_lo=%d clamp_mid=%d clamp_hi=%d\n",
         CLAMP(x - 9, 3, 20), CLAMP(x + 1, 3, 20), CLAMP(x * y, 3, 20));
  STEP(x);
  STEP(x);
  printf("step=%d\n", x);
  printf("accum=%d\n", accum(9));
  printf("label=%d\n", label_loop(5));
  int z = 1 ? MAXV(x, y) : (x / (y - y));
  printf("z=%d\n", z);
  printf("final=%d\n", MINV(z, 6));
  return 0;
}
