// FR-40 on C++ input (W2.0 subset). Pins the namespace-flattened node keys
// the importer emits — `ns_<name>_` per level, composing for nested
// namespaces, and the fixed `ns_anon_` tag for an anonymous namespace — the
// transparency of `extern "C" { ... }` (a LinkageSpecDecl contributes
// nothing to a name, matching C linkage), and that a `class` is a record
// node whose direct base class is a `Base` edge.
// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s

class Shape {
public:
  int tag;
};

class Circle : public Shape {
public:
  int radius;
};

namespace outer {
int width;
int area(int side) { return side * side; }

namespace inner {
int depth;
int volume(int side) { return area(side) * depth; }
} // namespace inner
} // namespace outer

namespace {
int hidden_count;
int bump() { return hidden_count + 1; }
} // namespace

extern "C" {
int plain_c(Circle *c) { return c->radius; }
}

int main(void) {
  Circle c;
  c.radius = outer::inner::volume(2) + outer::width + bump();
  return plain_c(&c);
}

// A `class` is a record node exactly like a `struct`; a namespace chain
// flattens into the symbol, and an anonymous namespace takes `ns_anon_`.
// `extern "C"` contributes nothing, so `plain_c` keeps its bare name.
// CHECK:      node Circle kind=record def=1 linkage=extern tu=0 loc={{.*}}item-graph-cpp.cpp:14:7
// CHECK-NEXT: node Shape kind=record def=1 linkage=extern tu=0 loc={{.*}}item-graph-cpp.cpp:9:7
// CHECK-NEXT: node c_main kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-cpp.cpp:38:5
// CHECK-NEXT: node ns_anon_bump kind=function def=1 linkage=intern tu=0 loc={{.*}}item-graph-cpp.cpp:31:5
// CHECK-NEXT: node ns_outer_area kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-cpp.cpp:21:5
// CHECK-NEXT: node ns_outer_ns_inner_depth kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-cpp.cpp:24:5
// CHECK-NEXT: node ns_outer_ns_inner_volume kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-cpp.cpp:25:5
// CHECK-NEXT: node ns_outer_width kind=global def=1 linkage=extern tu=0 loc={{.*}}item-graph-cpp.cpp:20:5
// CHECK-NEXT: node plain_c kind=function def=1 linkage=extern tu=0 loc={{.*}}item-graph-cpp.cpp:35:5

// An anonymous-namespace VARIABLE additionally takes the per-TU tag while a
// function in the same namespace does not: `globalVarSymbolName` keys off
// clang's `isExternallyVisible` (false in an anonymous namespace) whereas
// `mlirFuncName` keys off an explicit `static` storage class. The graph
// shares those exact functions, so it reproduces the importer's asymmetry
// rather than papering over it — the node key stays the emitted item name.
// CHECK-NEXT: node tu0_ns_anon_hidden_count kind=global def=1 linkage=intern tu=0 loc={{.*}}item-graph-cpp.cpp:30:5

// The base class is a Base edge, not a Field one.
// CHECK-NEXT: edge Circle -> Shape kind=Base
// CHECK-NEXT: edge c_main -> ns_anon_bump kind=Calls
// CHECK-NEXT: edge c_main -> ns_outer_ns_inner_volume kind=Calls
// CHECK-NEXT: edge c_main -> plain_c kind=Calls
// CHECK-NEXT: edge c_main -> Circle kind=BodyType
// CHECK-NEXT: edge c_main -> ns_outer_width kind=ReadsGlobal
// CHECK-NEXT: edge ns_anon_bump -> tu0_ns_anon_hidden_count kind=ReadsGlobal
// CHECK-NEXT: edge ns_outer_ns_inner_volume -> ns_outer_area kind=Calls
// CHECK-NEXT: edge ns_outer_ns_inner_volume -> ns_outer_ns_inner_depth kind=ReadsGlobal
// CHECK-NEXT: edge plain_c -> Circle kind=SigType
// CHECK-NOT:  edge
