// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/copy-default.cpp 2>&1 | FileCheck %s --check-prefix=COPYDEFAULT
// RUN: not emitrust-import-c %t/move-default.cpp 2>&1 | FileCheck %s --check-prefix=MOVEDEFAULT

// Skipping an EXPLICITLY defaulted special member (see cpp-defaulted-ctor.cpp)
// must not extend to a defaulted COPY or MOVE constructor: those carry no
// value/aliasing semantics in the model and stay rejected — located — rather
// than being silently accepted as a trivial copy. The rejection loop runs on
// the broader "user-declared" predicate for exactly this reason.

//--- copy-default.cpp
// COPYDEFAULT: copy-default.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: copy/move/delegating constructor
struct C {
  int x;
  C(int v) { x = v; }
  C(const C &) = default;
};

int use(C a) {
  C c(3);
  return c.x + a.x;
}

//--- move-default.cpp
// MOVEDEFAULT: move-default.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: copy/move/delegating constructor
struct D {
  int x;
  D(int v) { x = v; }
  D(D &&) = default;
};

int use2() {
  D d(3);
  return d.x;
}
