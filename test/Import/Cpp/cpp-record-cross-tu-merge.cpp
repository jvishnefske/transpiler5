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

// ---------------------------------------------------------------------------
// FR-122 legs. The dedup key now folds in the ODR hash for records with a
// member surface (user-declared dtor or any non-implicit method), so these
// pins say the LEGITIMATE mergers keep merging under the sharpened key:
//
//  * DROPPY -- a shared-header class carrying a DESTRUCTOR (plus a method).
//    No pinned case carried a dtor before this wave -- that gap is exactly
//    how the FR-122 defect hid: colliding a droppy record with a POD twin
//    silently gained or lost the `impl Drop` depending on TU order. Two
//    TUs including ONE droppy header must still resolve to one struct_def
//    (with has_drop), one dtor body, and one method body -- and this merge
//    surviving proves getODRHash is stable across separate per-TU
//    ASTContexts.
//  * TWIN -- a pure field-only POD spelled out INDEPENDENTLY in two TUs (no
//    shared header). Gate (b) keeps PODs on the shape-only key, so the
//    benign twin merge -- and with it all of C -- is untouched.
//  * ALIAS -- W2.16's samePattern aliasing, `Box<char>` in one TU and
//    `Box<signed char>` in another, both coding `i8`. The two
//    specializations hash DIFFERENTLY, so this merge survives only
//    because gate (a) exempts ClassTemplateSpecializationDecl from the
//    ODR suffix -- this leg is the pin that keeps that exemption honest.
// RUN: emitrust-import-c %t/drop-a.cpp %t/drop-b.cpp | FileCheck %s --check-prefix=DROPPY
// RUN: emitrust-import-c %t/drop-a.cpp %t/drop-b.cpp | FileCheck %s --check-prefix=DROPPYFN
// RUN: emitrust-import-c %t/twin-a.cpp %t/twin-b.cpp | FileCheck %s --check-prefix=TWIN
// RUN: emitrust-import-c %t/alias-a.cpp %t/alias-b.cpp | FileCheck %s --check-prefix=ALIAS

//--- drop.h
#pragma once
extern "C" int printf(const char *, ...);
struct Keeper {
  int v;
  ~Keeper() { printf("keeper out %d\n", v); }
  int get() const { return v; }
};

//--- drop-a.cpp
#include "drop.h"
int use_k1(int x) {
  Keeper k;
  k.v = x + 3;
  return k.get();
}

//--- drop-b.cpp
#include "drop.h"
int use_k1(int);
int main(int argc, char **argv) {
  Keeper k;
  k.v = argc + 8;
  return k.get() + use_k1(argc);
}

// One struct_def WITH the drop marker, one Drop body, one method body.
// DROPPY-COUNT-1: emitrust.struct_def @Keeper ["v"] [i32] {{.*}}emitrust.has_drop}
// DROPPY-NOT: emitrust.struct_def @Keeper
// DROPPYFN-COUNT-1: func.func @Keeper_dtor
// DROPPYFN-NOT: func.func @Keeper_dtor
// DROPPYFN-COUNT-1: func.func @Keeper_get
// DROPPYFN-NOT: func.func @Keeper_get

//--- twin-a.cpp
struct Twin {
  int a;
  int b;
};
int use_p1(int x) {
  Twin t;
  t.a = x;
  t.b = x + 1;
  return t.a + t.b;
}

//--- twin-b.cpp
struct Twin {
  int a;
  int b;
};
int use_p1(int);
int main(int argc, char **argv) {
  Twin t;
  t.a = argc + 2;
  t.b = argc + 3;
  return t.a + t.b + use_p1(argc);
}

// TWIN-COUNT-1: emitrust.struct_def @Twin ["a", "b"] [i32, i32]
// TWIN-NOT: emitrust.struct_def @Twin

//--- alias.h
#pragma once
template <typename T>
struct Box {
  T v;
  Box(T x) : v(x) {}
  T get() const { return v; }
};

//--- alias-a.cpp
#include "alias.h"
int use_a(int x) {
  Box<char> b((char)x);
  return (int)b.get();
}

//--- alias-b.cpp
#include "alias.h"
int use_a(int);
int main(int argc, char **argv) {
  Box<signed char> b((signed char)argc);
  return (int)b.get() + use_a(argc);
}

// ALIAS-COUNT-1: emitrust.struct_def @Box_i8 ["v"] [i8]
// ALIAS-NOT: emitrust.struct_def @Box_i8
// ALIAS-NOT: emitrust.struct_def @Box
