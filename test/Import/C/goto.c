// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | emitrust-opt --mem2reg --canonicalize --lift-cf-to-scf --canonicalize | FileCheck %s --check-prefix=SCF

// C99-33: goto/label support. Labels map to dedicated blocks (created on
// first mention, so forward and backward gotos both resolve); a goto is a
// plain cf.br and the trailing dead code lands in an unreachable block that
// function finalization erases. lift-cf-to-scf then structures the CFG.

int forward(int x) {
  if (x)
    goto out;
  x = x + 1;
out:
  return x;
}

// The goto branches straight to the label block; no cf remains after lifting.
// CHECK-LABEL: func.func @forward
// CHECK: cf.cond_br
// CHECK: cf.br ^[[OUT:bb[0-9]+]]
// CHECK: ^[[OUT]]:
// CHECK: return
// SCF-LABEL: func.func @forward
// SCF-NOT: cf.br

int backward(int n) {
  int acc = 0;
top:
  acc = acc + n;
  n = n - 1;
  if (n > 0)
    goto top;
  return acc;
}

// A backward goto forms a loop: the label block has two predecessors and
// lifts to a structured scf.while.
// CHECK-LABEL: func.func @backward
// CHECK: cf.br ^[[TOP:bb[0-9]+]]
// CHECK: ^[[TOP]]:
// CHECK: cf.cond_br %{{.*}}, ^{{bb[0-9]+}}, ^{{bb[0-9]+}}
// SCF-LABEL: func.func @backward
// SCF: scf.while
// SCF-NOT: cf.br

int escape_nested(int limit) {
  int hits = 0;
  for (int i = 0; i < 10; i = i + 1) {
    for (int j = 0; j < 10; j = j + 1) {
      hits = hits + 1;
      if (hits == limit)
        goto done;
    }
  }
done:
  return hits;
}

// A goto out of two nested loops is an ordinary branch to the label block.
// CHECK-LABEL: func.func @escape_nested
// SCF-LABEL: func.func @escape_nested
// SCF-NOT: cf.br

int into_loop(void) {
  int i = 0;
  int n = 0;
  goto inside;
  while (i < 5) {
  inside:
    n = n + 2;
    i = i + 1;
  }
  return n;
}

// A goto into a loop body (irreducible CFG) still lifts: transformCFGToSCF
// multiplexes the extra entry edge into a structured loop.
// CHECK-LABEL: func.func @into_loop
// SCF-LABEL: func.func @into_loop
// SCF-NOT: cf.br

unsigned skip_init(void) {
  goto skip;
  unsigned u = 9u;
skip:
  u = 3u;
  return u;
}

// A goto over a declaration must not leave the later use undominated: in a
// function with labels the emitrust.variable place is hoisted to the entry
// block, before any branch.
// CHECK-LABEL: func.func @skip_init
// CHECK: emitrust.variable named "u" : !emitrust.lvalue<ui32>
// CHECK: cf.br
// SCF-LABEL: func.func @skip_init

int fallthrough_label(int x) {
  x = x + 1;
middle:
  x = x + 2;
  if (x < 10)
    goto middle;
  return x;
}

// A label reached by falling through gets an explicit branch into its block.
// CHECK-LABEL: func.func @fallthrough_label
// SCF-LABEL: func.func @fallthrough_label
// SCF-NOT: cf.br
