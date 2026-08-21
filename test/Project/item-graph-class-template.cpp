// W2.16 on the FR-40 whole-program index. A class template's emitted
// items are its INSTANTIATIONS, not the uninstantiated pattern, so the
// index must key one RECORD node per instantiation under the SAME
// suffixed symbol the importer emits (CSymbolNaming.h's `recordRustName`
// is the single source of truth both sides call — that is the CLAUDE.md
// byte-identity invariant), and a body that names an instantiation must
// carry its `BodyType` edge to that same node.
//
// This pins the half that was silently missing before W2.16 and is easy
// to regress: without the `collectItems` arm, `--emit=item-graph` over a
// class-template input reported ONLY `c_main` (measured), so
// `--incremental`'s PORTING.md / emitrust-progress.json DENOMINATOR
// silently understated the work — exactly the gap W2.15 closed for
// function templates.
//
// The two distinct nodes are the load-bearing part: `Box<int>` and
// `Box<double>` are two structs, not one, and the item graph must agree
// with the importer about both spellings. Note these are the POST-rename
// (`emitrust-cc`) spellings — `BoxI32`, not `Box_i32` — which is the
// other half of the naming contract: the template-argument suffix is
// appended BEFORE `toUpperCamelCase`, so the composed name survives
// rustc's denied `non_camel_case_types` lint.
// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s

extern "C" int printf(const char *, ...);

template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  T get() const { return v; }
};

// A record-typed template argument, so the instantiation has an outgoing
// FIELD edge of its own: without the `collectDependencies` arm the node
// would exist with no edge to `Point`, and the coloring pass could not
// propagate an inadmissible field type through an instantiation.
struct Point {
  int x;
};

template <typename T>
struct Wrap {
  T v;
};

int main() {
  Box<int> bi(4);
  Box<double> bd(0.5);
  Wrap<Point> w;
  w.v.x = 3;
  printf("%d %.2f %d\n", bi.get(), bd.get(), w.v.x);
  return 0;
}

// One node per instantiation, none for the pattern.
// CHECK-DAG: node BoxI32 kind=record def=1
// CHECK-DAG: node BoxD kind=record def=1
// CHECK-DAG: edge c_main -> BoxI32 kind=BodyType
// CHECK-DAG: edge c_main -> BoxD kind=BodyType
// CHECK-DAG: node WrapPoint kind=record def=1
// CHECK-DAG: edge WrapPoint -> Point kind=Field

// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s --check-prefix=NOPATTERN
// The uninstantiated pattern is not an item: there is no node under the
// bare template spelling, and no body edge to one.
// NOPATTERN-NOT: node Box kind=
// NOPATTERN-NOT: edge c_main -> Box kind=
