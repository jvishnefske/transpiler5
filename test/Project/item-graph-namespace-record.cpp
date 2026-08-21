// FR-108 (FR-40 half): the item graph's record node KEY is the emitted
// record symbol, recomputed from the AST alone by
// `ItemGraphBuilder::recordSymbolFor`. Before FR-108 gave record names
// the `namespacePrefix` that function names have always had, `::Box` and
// `ns::Box` computed the identical key and the graph emitted ONE record
// node for TWO records — the graph silently agreed with the importer's
// silent merge instead of exposing it.
//
// This pins that the two now yield TWO nodes under distinct keys, and
// that every edge (field, signature, body type) attaches to the right
// one. It is the FR-40 side of the same byte-identity invariant the
// importer golden `test/Import/Cpp/cpp-namespace-records.cpp` pins.
// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s

struct Box {
  int v;
};

namespace ns {
struct Box {
  int v;
};
} // namespace ns

namespace a {
namespace b {
struct Pt {
  int x;
};
} // namespace b
} // namespace a

int read_global(Box *b) { return b->v; }

namespace ns {
int read_ns(Box *b) { return b->v; }
} // namespace ns

int main(void) {
  Box g;
  ns::Box n;
  a::b::Pt p;
  g.v = 1;
  n.v = 2;
  p.x = 3;
  return read_global(&g) + ns::read_ns(&n) + p.x;
}

// Two record nodes for two records, keyed by the prefixed emitted name.
// CHECK:      node Box kind=record def=1 linkage=extern tu=0 loc={{.*}}item-graph-namespace-record.cpp:15:8
// CHECK-NEXT: node NsANsBPt kind=record def=1 linkage=extern tu=0 loc={{.*}}item-graph-namespace-record.cpp:27:8
// CHECK-NEXT: node NsNsBox kind=record def=1 linkage=extern tu=0 loc={{.*}}item-graph-namespace-record.cpp:20:8
// CHECK-NEXT: node c_main kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-namespace-record.cpp:39:5
// CHECK-NEXT: node ns_ns_read_ns kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-namespace-record.cpp:36:5
// CHECK-NEXT: node read_global kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-namespace-record.cpp:33:5

// Every edge lands on the namespace-correct node: `read_global` takes the
// global `Box`, `ns::read_ns` takes `ns::Box`, and `main` bodies all three.
// CHECK-NEXT: edge c_main -> ns_ns_read_ns kind=Calls
// CHECK-NEXT: edge c_main -> read_global kind=Calls
// CHECK-NEXT: edge c_main -> Box kind=BodyType
// CHECK-NEXT: edge c_main -> NsANsBPt kind=BodyType
// CHECK-NEXT: edge c_main -> NsNsBox kind=BodyType
// CHECK-NEXT: edge ns_ns_read_ns -> NsNsBox kind=SigType
// CHECK-NEXT: edge read_global -> Box kind=SigType
// CHECK-NOT:  edge
