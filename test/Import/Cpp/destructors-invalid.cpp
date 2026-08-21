// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/virtual-dtor.cpp 2>&1 | FileCheck %s --check-prefix=VIRTDTOR
// RUN: not emitrust-import-c %t/no-body.cpp 2>&1 | FileCheck %s --check-prefix=NOBODY
// RUN: not emitrust-import-c %t/union-dtor.cpp 2>&1 | FileCheck %s --check-prefix=UNIONDTOR
// RUN: not emitrust-import-c %t/dtor-name-clash.cpp 2>&1 | FileCheck %s --check-prefix=DTORCLASH
// RUN: not emitrust-import-c %t/drop-member.cpp 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: not emitrust-import-c %t/drop-member-array.cpp 2>&1 | FileCheck %s --check-prefix=MEMBERARRAY
// RUN: not emitrust-import-c %t/drop-array.cpp 2>&1 | FileCheck %s --check-prefix=ARRAY
// RUN: not emitrust-import-c %t/drop-global.cpp 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not emitrust-import-c %t/drop-static-local.cpp 2>&1 | FileCheck %s --check-prefix=STATICLOCAL
// RUN: not emitrust-import-c %t/drop-byvalue-param.cpp 2>&1 | FileCheck %s --check-prefix=BYVALPARAM
// RUN: not emitrust-import-c %t/drop-byvalue-return.cpp 2>&1 | FileCheck %s --check-prefix=BYVALRET
// RUN: not emitrust-import-c %t/drop-nested-block.cpp 2>&1 | FileCheck %s --check-prefix=NESTEDBLOCK
// RUN: not emitrust-import-c %t/drop-switch-case.cpp 2>&1 | FileCheck %s --check-prefix=SWITCHCASE
// RUN: not emitrust-import-c %t/drop-for-init.cpp 2>&1 | FileCheck %s --check-prefix=FORINIT
// RUN: not emitrust-import-c %t/drop-goto.cpp 2>&1 | FileCheck %s --check-prefix=GOTO
// RUN: not emitrust-import-c %t/drop-for-inc-call.cpp 2>&1 | FileCheck %s --check-prefix=FORINCCALL
// RUN: emitrust-cc --recover --emit=rust %t/drop-nested-block.cpp | FileCheck %s --check-prefix=NESTEDBLOCKREC --implicit-check-not="after inner"
// RUN: emitrust-cc --recover --emit=rust %t/drop-nested-block.cpp 2>&1 >/dev/null | FileCheck %s --check-prefix=NESTEDBLOCKRECDIAG

// W2.17 located-rejection ledger for user-declared destructors. The wave
// admits exactly ONE shape -- a non-virtual destructor, defined in this
// translation unit, on a class with no base classes, every object of
// which is a LOCAL whose `DeclStmt` sits directly in a function body, a
// loop body, or an if/else branch (see destructors.cpp). That subset was
// chosen because it is the subset that byte-diffs clean against
// `clang++ -std=c++17`; EVERY other shape below was MEASURED to diverge,
// and each divergence is a SILENT MISCOMPILE (the crate compiles clean
// and prints different bytes), not a loud failure. This file is the
// ledger that keeps them loud.
//
// The measured divergences, one per case:
//
// * bare nested block / switch case / for-init: the importer FLATTENS a
//   bare `CompoundStmt` (no scope/block op exists in the dialect), and
//   hoists a for-init declaration out of the loop, so the object outlives
//   its C++ lifetime and the drop moves LATER in the output.
// * a `goto`/label in the function: a local is hoisted to function top,
//   so the goto-taken path constructs nothing in C++ but drops in Rust.
// * a destructor-carrying struct MEMBER -- C++ destroys members in REVERSE
//   declaration order, Rust drops fields in FORWARD order.
// * an ARRAY of such objects: C++ destroys in reverse index order, Rust
//   in forward index order -- and `[X; N]` repeat is rustc E0277 anyway
//   (`Copy` is gone from the derive, see the has_drop pin).
// * a global/static object: C++ destroys it at exit, a Rust `static`
//   never drops at all, and the emitter localizes such a global into a
//   function-scope binding whose drop then runs at the WRONG time.
// * by value across a call or return: C++ destroys the callee's parameter
//   COPY as well as the caller's object (two runs of the destructor);
//   Rust moves (one run). The subset models no copy constructor, so there
//   is nothing that could make the counts agree.
// * a for-loop whose INCREMENT can have side effects: C++ destroys the
//   body's locals BEFORE evaluating the increment; the importer lowers a
//   general `for` into a `while` with the increment at the BOTTOM OF THE
//   BODY, i.e. before the drop. Measured swap of `inc`/`dtor` lines.
// * a virtual destructor: no vtable/dynamic dispatch is modeled at all.
//   Checked BEFORE the general destructor admission so the diagnostic
//   names the actual blocker (a virtual `~Shape()` used to report the
//   generic `user-declared destructor`).
// * a destructor with no definition in THIS translation unit: an
//   uncalled, undefined method is silently DROPPED from emission, and
//   nothing ever calls a destructor -- so the class would emit with NO
//   `impl Drop` at all and lose every side effect silently.
//
// The generic `unsupported: user-declared destructor` wording is RETAINED
// (and still tabulated as `cxx-destructor`) for the residual shape a
// union destructor is; test/Import/Cpp/methods-invalid.cpp's `destructor`
// case moved FORWARD to that shape rather than loosening.

//--- virtual-dtor.cpp
// VIRTDTOR: virtual-dtor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual destructor
extern "C" int printf(const char *, ...);
struct S {
  int id;
  virtual ~S() { printf("dtor %d\n", id); }
};
int use(void) {
  S s;
  s.id = 1;
  return s.id;
}

//--- no-body.cpp
// NOBODY: no-body.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: destructor with no definition in this translation unit
extern "C" int printf(const char *, ...);
struct S {
  int id;
  ~S();
};
int use(void) {
  S s;
  s.id = 1;
  return s.id;
}

//--- union-dtor.cpp
// UNIONDTOR: union-dtor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: user-declared destructor
extern "C" int printf(const char *, ...);
union U {
  int i;
  ~U() { printf("dtor\n"); }
};
int use(void) {
  U u;
  u.i = 1;
  return u.i;
}

//--- dtor-name-clash.cpp
// The destructor's module symbol is `<Struct>_dtor`, so a member function
// literally spelled `dtor` would land on the same symbol -- and the
// overload-suffix counter deliberately SKIPS destructors, so it could not
// disambiguate them either. (A member spelled `drop` is fine and is
// exercised positively in test/EndToEnd/cpp-destructor.cpp: it takes
// `<Struct>_drop`, which is precisely why the destructor does not.)
// DTORCLASH: dtor-name-clash.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: destructor collides with the member function 'dtor'
extern "C" int printf(const char *, ...);
struct S {
  int id;
  void dtor() { printf("manual %d\n", id); }
  ~S() { printf("dtor %d\n", id); }
};
int use(void) {
  S s;
  s.id = 1;
  s.dtor();
  return s.id;
}

//--- drop-member.cpp
// MEMBER: drop-member.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct member of a class with a destructor
extern "C" int printf(const char *, ...);
struct Inner {
  int id;
  ~Inner() { printf("dtor %d\n", id); }
};
struct Outer {
  int n;
  Inner in;
};
int use(void) {
  Outer o;
  o.n = 1;
  return o.n;
}

//--- drop-member-array.cpp
// An ARRAY-typed member of a destructor-carrying class is the same member
// hazard: the element type is what decides, not the array wrapper.
// MEMBERARRAY: drop-member-array.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct member of a class with a destructor
extern "C" int printf(const char *, ...);
struct Inner {
  int id;
  ~Inner() { printf("dtor %d\n", id); }
};
struct Outer {
  int n;
  Inner in[2];
};
int use(void) {
  Outer o;
  o.n = 1;
  return o.n;
}

//--- drop-array.cpp
// ARRAY: drop-array.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: array of a class with a destructor
extern "C" int printf(const char *, ...);
struct S {
  int id;
  ~S() { printf("dtor %d\n", id); }
};
int use(void) {
  S a[3];
  a[0].id = 1;
  return a[0].id;
}

//--- drop-global.cpp
// GLOBAL: drop-global.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global or static object of a class with a destructor
extern "C" int printf(const char *, ...);
struct S {
  int id;
  ~S() { printf("dtor %d\n", id); }
};
S g;
int use(void) {
  g.id = 1;
  return g.id;
}

//--- drop-static-local.cpp
// A function-local `static` is module-level state with the same
// never-dropped hazard as a file-scope global.
// STATICLOCAL: drop-static-local.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global or static object of a class with a destructor
extern "C" int printf(const char *, ...);
struct S {
  int id;
  ~S() { printf("dtor %d\n", id); }
};
int use(void) {
  static S s;
  s.id = 1;
  return s.id;
}

//--- drop-byvalue-param.cpp
// BYVALPARAM: drop-byvalue-param.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: class with a destructor passed or returned by value
extern "C" int printf(const char *, ...);
struct S {
  int id;
  ~S() { printf("dtor %d\n", id); }
};
int take(S s) { return s.id; }
int use(void) {
  S a;
  a.id = 1;
  return take(a);
}

//--- drop-byvalue-return.cpp
// BYVALRET: drop-byvalue-return.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: class with a destructor passed or returned by value
extern "C" int printf(const char *, ...);
struct S {
  int id;
  ~S() { printf("dtor %d\n", id); }
};
S make(int v) {
  S s;
  s.id = v;
  return s;
}
int use(void) { return make(3).id; }

//--- drop-nested-block.cpp
// NESTEDBLOCK: drop-nested-block.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: object of a class with a destructor outside a function, loop, or branch body
extern "C" int printf(const char *, ...);
struct S {
  int id;
  ~S() { printf("dtor %d\n", id); }
};
int use(void) {
  {
    S b;
    b.id = 11;
  }
  printf("after inner\n");
  return 0;
}
// FR-42 recovery composes. The CLASS is admissible -- only this USE of it
// is not -- so `impl Drop for S` still reaches the output; what is refused
// is the FUNCTION, which becomes an `unimplemented!()` stub carrying the
// rejection wording verbatim. Nothing from its body (the misplaced object,
// the `after inner` print) survives, so no wrong drop point can be emitted.
// NESTEDBLOCKREC: pub fn use_() -> i32 {
// NESTEDBLOCKREC-NEXT: unimplemented!("unsupported: object of a class with a destructor outside a function, loop, or branch body")
// NESTEDBLOCKREC: impl Drop for S {
//
// And it tabulates under the wave's new blocker tag, which is what makes
// the front rankable in the RealWorld/progress-JSON survey.
// NESTEDBLOCKRECDIAG: warning: unsupported: object of a class with a destructor outside a function, loop, or branch body
// NESTEDBLOCKRECDIAG: stubbed 'use_' [cxx-drop-scope]
// NESTEDBLOCKRECDIAG: cxx-drop-scope 1

//--- drop-switch-case.cpp
// SWITCHCASE: drop-switch-case.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: object of a class with a destructor outside a function, loop, or branch body
extern "C" int printf(const char *, ...);
struct S {
  int id;
  ~S() { printf("dtor %d\n", id); }
};
int use(int n) {
  switch (n) {
  case 1: {
    S b;
    b.id = 11;
    break;
  }
  default:
    break;
  }
  return 0;
}

//--- drop-for-init.cpp
// FORINIT: drop-for-init.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: object of a class with a destructor outside a function, loop, or branch body
extern "C" int printf(const char *, ...);
struct S {
  int id;
  ~S() { printf("dtor %d\n", id); }
};
int use(void) {
  for (S k; k.id < 2; k.id = k.id + 1)
    printf("iter %d\n", k.id);
  return 0;
}

//--- drop-goto.cpp
// GOTO: drop-goto.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: object of a class with a destructor outside a function, loop, or branch body
extern "C" int printf(const char *, ...);
struct S {
  int id;
  ~S() { printf("dtor %d\n", id); }
};
int use(int n) {
  S a;
  a.id = n;
  if (n > 0)
    goto out;
  printf("mid %d\n", a.id);
out:
  return 0;
}

//--- drop-for-inc-call.cpp
// FORINCCALL: drop-for-inc-call.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: object of a class with a destructor in a loop whose increment has side effects
extern "C" int printf(const char *, ...);
static int bump(int i) { printf("inc %d\n", i); return i + 1; }
struct S {
  int id;
  ~S() { printf("dtor %d\n", id); }
};
int use(void) {
  for (int i = 0; i < 2; i = bump(i)) {
    S c;
    c.id = i;
  }
  return 0;
}
