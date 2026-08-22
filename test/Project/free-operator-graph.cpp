// RUN: emitrust-cc --emit=item-graph %s -o - \
// RUN:   | FileCheck %s --check-prefix=GRAPH --implicit-check-not="node kind"
// RUN: emitrust-cc --emit=coloring %s -o - \
// RUN:   | FileCheck %s --check-prefix=COLOR --implicit-check-not="item kind"

// FR-119, graph side: ItemGraph used to node a free operator through
// `cFunctionSymbolName` -> key "" (the getName() assert compiles out under
// NDEBUG), so the TWO operators below deduped into ONE empty-named node
// (`node  kind=function ...`) and ItemColoring printed a false GREEN with
// an empty name. Both walkers now screen non-identifier free FunctionDecls
// AST-pure -- the isIdentifier test runs BEFORE cFunctionSymbolName, which
// also closes the latent debug-build assert path -- mirroring the
// tuOrdinaryNames collector that already had the screen. The
// --implicit-check-not patterns above pin the empty-named node and the
// empty-named item absent (FileCheck's whitespace canonicalization makes
// "node kind" match the `node  kind` the defect printed); the tally pins
// the honest count: exactly A and c_main, green=2.

// GRAPH: node A kind=record def=1
// GRAPH: node c_main kind=function def=1
// GRAPH: edge c_main -> A kind=BodyType

// COLOR: item A kind=record color=green reason=admissible
// COLOR: item c_main kind=function color=green reason=admissible
// COLOR: tally green=2 yellow=0 red=0

struct A {
  int v;
};
int operator+(A a, int b) { return a.v + b; }
int operator-(A a, int b) { return a.v - b; }
int main(void) {
  A a;
  a.v = 3;
  return a.v;
}
