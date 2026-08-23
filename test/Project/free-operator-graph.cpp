// RUN: emitrust-cc --emit=item-graph %s -o - \
// RUN:   | FileCheck %s --check-prefix=GRAPH --implicit-check-not="node kind" \
// RUN:     --implicit-check-not="op_shl"
// RUN: emitrust-cc --emit=coloring %s -o - \
// RUN:   | FileCheck %s --check-prefix=COLOR --implicit-check-not="item kind" \
// RUN:     --implicit-check-not="op_shl"

// FR-119, graph side: ItemGraph used to node a free operator through
// `cFunctionSymbolName` -> key "" (the getName() assert compiles out under
// NDEBUG), so two free operators deduped into ONE empty-named node
// (`node  kind=function ...`) and ItemColoring printed a false GREEN with
// an empty name. W2.25 moved the pin FORWARD: a free operator of an
// ADMITTED kind now composes its synthesized identifier spelling
// (`operator+` -> op_add, `operator-` -> op_sub) and mints a REAL node
// with a real verdict, in lockstep with the importer's admission — the
// graph and the emitted module agree on the item set. The original
// invariant is intact for everything outside the table: the NON-admitted
// `operator<<` below still composes the EMPTY symbol, mints no node, and
// gets no verdict (`cFunctionSymbolName` screens the DeclarationName
// itself, so the latent debug-build getName() assert stays closed). The
// --implicit-check-not patterns pin the empty-named node/item AND any
// `op_shl` leak absent; the tallies pin the honest counts.

// GRAPH: node A kind=record def=1
// GRAPH: node c_main kind=function def=1
// GRAPH: node op_add kind=function def=1
// GRAPH: node op_sub kind=function def=1
// GRAPH: edge c_main -> A kind=BodyType
// GRAPH: edge op_add -> A kind=SigType
// GRAPH: edge op_sub -> A kind=SigType

// COLOR: item A kind=record color=green reason=admissible
// COLOR: item c_main kind=function color=green reason=admissible
// COLOR: item op_add kind=function color=green reason=admissible
// COLOR: item op_sub kind=function color=green reason=admissible
// COLOR: tally green=4 yellow=0 red=0

struct A {
  int v;
};
int operator+(A a, int b) { return a.v + b; }
int operator-(A a, int b) { return a.v - b; }
int operator<<(A a, int b) { return a.v << b; }
int main(void) {
  A a;
  a.v = 3;
  return a.v;
}
