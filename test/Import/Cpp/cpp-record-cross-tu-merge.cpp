// FR-108 GUARD RAIL. The same-TU record-name clash guard added by FR-108
// sits on the code path that ALSO serves the legitimate cross-TU merge:
// two translation units including ONE header must resolve to ONE emitted
// struct and ONE impl, or the FR-58 shard merge would emit the type twice
// and rustc would refuse the crate (E0428). The discriminator is DECL
// IDENTITY WITHIN ONE TU (`structNameOwnerTuTags`), not name reuse, and
// this file is the pin that says the discriminator still discriminates.
//
// All three record shapes that travel the path are covered: a plain
// struct, a struct inside a namespace (whose emitted name now carries
// FR-108's `ns_ns_` prefix and so must merge under the PREFIXED name),
// and a class-template instantiation (W2.16's original case, previously
// unpinned by any lit test).
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/plain-a.cpp %t/plain-b.cpp | FileCheck %s --check-prefix=PLAIN
// RUN: emitrust-import-c %t/plain-a.cpp %t/plain-b.cpp | FileCheck %s --check-prefix=PLAINFN
// RUN: emitrust-import-c %t/ns-a.cpp %t/ns-b.cpp | FileCheck %s --check-prefix=NS
// RUN: emitrust-import-c %t/ns-a.cpp %t/ns-b.cpp | FileCheck %s --check-prefix=NSFN
// RUN: emitrust-import-c %t/tmpl-a.cpp %t/tmpl-b.cpp | FileCheck %s --check-prefix=TMPL
// RUN: emitrust-import-c %t/tmpl-a.cpp %t/tmpl-b.cpp | FileCheck %s --check-prefix=TMPLFN

//--- plain.h
#pragma once
struct Box {
  int v;
  int get() const { return v; }
};

//--- plain-a.cpp
#include "plain.h"
int from_a(int x) {
  Box b;
  b.v = x;
  return b.get();
}

//--- plain-b.cpp
#include "plain.h"
int from_a(int);
int main(int argc, char **argv) {
  Box b;
  b.v = argc + 3;
  return b.get() + from_a(argc);
}

// One struct_def and one method body for the header's struct, however
// many TUs included it.
// PLAIN-COUNT-1: emitrust.struct_def @Box ["v"] [i32]
// PLAIN-NOT: emitrust.struct_def @Box
// PLAINFN-COUNT-1: func.func @Box_get
// PLAINFN-NOT: func.func @Box_get

//--- ns.h
#pragma once
namespace ns {
struct Box {
  int v;
  int get() const { return v + 100; }
};
} // namespace ns

//--- ns-a.cpp
#include "ns.h"
int from_a(int x) {
  ns::Box b;
  b.v = x;
  return b.get();
}

//--- ns-b.cpp
#include "ns.h"
int from_a(int);
int main(int argc, char **argv) {
  ns::Box b;
  b.v = argc + 3;
  return b.get() + from_a(argc);
}

// The merge happens under the FR-108 PREFIXED name; no bare `Box` is
// emitted alongside it.
// NS-COUNT-1: emitrust.struct_def @ns_ns_Box ["v"] [i32]
// NS-NOT: emitrust.struct_def @ns_ns_Box
// NS-NOT: emitrust.struct_def @Box
// NSFN-COUNT-1: func.func @ns_ns_Box_get
// NSFN-NOT: func.func @ns_ns_Box_get

//--- tmpl.h
#pragma once
template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  T get() const { return v; }
};

//--- tmpl-a.cpp
#include "tmpl.h"
int from_a(int x) {
  Box<int> b(x);
  return b.get();
}

//--- tmpl-b.cpp
#include "tmpl.h"
int from_a(int);
int main(int argc, char **argv) {
  Box<int> b(argc + 3);
  return b.get() + from_a(argc);
}

// Two TUs instantiating the SAME pattern at the SAME argument are one
// emitted type; the same-TU clash guard must not see two decls here.
// TMPL-COUNT-1: emitrust.struct_def @Box_i32 ["v"] [i32]
// TMPL-NOT: emitrust.struct_def @Box_i32
// TMPLFN-COUNT-1: func.func @Box_i32_get
// TMPLFN-NOT: func.func @Box_i32_get
