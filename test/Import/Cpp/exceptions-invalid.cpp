// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/uncaught-throw.cpp 2>&1 | FileCheck %s --check-prefix=UNCAUGHTTHROW
// RUN: not emitrust-import-c %t/uncaught-call.cpp 2>&1 | FileCheck %s --check-prefix=UNCAUGHTCALL
// RUN: not emitrust-import-c %t/method-throw.cpp 2>&1 | FileCheck %s --check-prefix=METHODTHROW
// RUN: not emitrust-import-c %t/method-throw-only.cpp 2>&1 | FileCheck %s --check-prefix=METHODONLY
// RUN: not emitrust-import-c %t/ctor-throw.cpp 2>&1 | FileCheck %s --check-prefix=CTORTHROW
// RUN: not emitrust-import-c %t/noexcept-throw.cpp 2>&1 | FileCheck %s --check-prefix=NOEXCEPT
// RUN: not emitrust-import-c %t/two-payloads.cpp 2>&1 | FileCheck %s --check-prefix=TWOPAYLOADS
// RUN: not emitrust-import-c %t/class-payload.cpp 2>&1 | FileCheck %s --check-prefix=CLASSPAYLOAD
// RUN: not emitrust-import-c %t/catch-by-ref.cpp 2>&1 | FileCheck %s --check-prefix=CATCHBYREF
// RUN: not emitrust-import-c %t/catch-mismatch.cpp 2>&1 | FileCheck %s --check-prefix=CATCHMISMATCH
// RUN: not emitrust-import-c %t/rethrow-outside-catch.cpp 2>&1 | FileCheck %s --check-prefix=RETHROWOUT
// RUN: not emitrust-import-c %t/address-taken.cpp 2>&1 | FileCheck %s --check-prefix=ADDRTAKEN
// RUN: not emitrust-import-c %t/variadic-throw.cpp 2>&1 | FileCheck %s --check-prefix=VARIADICTHROW
// RUN: not emitrust-import-c %t/return-mismatch.cpp 2>&1 | FileCheck %s --check-prefix=RETMISMATCH
// RUN: not emitrust-import-c %t/pointer-param.cpp 2>&1 | FileCheck %s --check-prefix=PTRPARAM
// RUN: not emitrust-import-c %t/multi-handler.cpp 2>&1 | FileCheck %s --check-prefix=MULTIHANDLER
// RUN: not emitrust-import-c %t/nested-try.cpp 2>&1 | FileCheck %s --check-prefix=NESTEDTRY
// RUN: not emitrust-import-c %t/try-in-method.cpp 2>&1 | FileCheck %s --check-prefix=TRYINMETHOD
// RUN: not emitrust-import-c %t/droppy-in-try.cpp 2>&1 | FileCheck %s --check-prefix=DROPPYINTRY
// RUN: not emitrust-import-c %t/try-no-throw.cpp 2>&1 | FileCheck %s --check-prefix=TRYNOTHROW

// W2.24 located-rejection ledger for the exceptions wave. The wave admits
// exactly the wave-1 gate — TU-level functions, ONE thrown scalar payload
// type per TU, direct calls only in the can-throw closure, closure members
// returning the payload type over arithmetic parameters, catch by value of
// the payload type (or catch(...)), and every unprotected throw/call
// landing in a closure member — and EVERYTHING outside it must stay a
// LOCATED rejection, because the alternative is a silently dropped
// exception (design.md W2.24's highest-severity spike finding). Two pins
// here are behavior-preserving controls, not new wordings: a THROW-FREE
// try keeps the historical generic CXXTryStmt fallback (the throw plan
// never activates), and a method-throw-ONLY TU keeps the historical
// generic CXXThrowExpr wording under FR-112's member omission (the
// planner's recursive walk deliberately skips method bodies: a method
// never joins the closure, and its throw is loud at its own import, so no
// edge into a method can lose an exception).

//--- uncaught-throw.cpp
// An UNCAUGHT throw stays rejected: main can never rewrite its signature,
// and the stdio models diverge (native printf to a pipe is fully buffered
// and abort() does not flush; Rust println! flushes on the newline).
int main() {
  throw 3;
  return 0;
}
// UNCAUGHTTHROW: uncaught-throw.cpp:5:3: error: unsupported: throw outside a try block in a function that cannot propagate exceptions

//--- uncaught-call.cpp
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
int main() { return thrower(0); }
// UNCAUGHTCALL: uncaught-call.cpp:6:21: error: unsupported: call to a potentially-throwing function outside a try block in a function that cannot propagate exceptions

//--- method-throw.cpp
// A throw inside a METHOD in a TU whose throw plan is ACTIVE: the method
// never joins the closure, so its throw takes the cannot-propagate
// rejection, which FR-112's containment turns into the omission WARNING;
// the call site is the hard error. The pair is the pin — the loudness
// this frontier's safety argument rests on.
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
class S {
public:
  int get() {
    throw 3;
  }
};
int main() {
  S s;
  int ok = 0;
  try {
    ok = thrower(1);
  } catch (int e) {
    ok = e;
  }
  return s.get() + ok;
}
// METHODTHROW: method-throw.cpp:14:5: warning: unsupported: throw outside a try block in a function that cannot propagate exceptions (omitted: method 'get' of class 'S')
// METHODTHROW: method-throw.cpp:25:10: error: unsupported: call to unimported method 'S_get'

//--- method-throw-only.cpp
// The INACTIVE-plan control: when the only throw in the TU lives in a
// method body, the planner's walk (which skips method bodies by design)
// never activates, and the historical generic wording is preserved
// verbatim under the FR-112 omission.
class S {
public:
  int get() {
    throw 3;
  }
};
int main() {
  S s;
  return s.get();
}
// METHODONLY: method-throw-only.cpp:8:5: warning: unsupported expression: CXXThrowExpr (omitted: method 'get' of class 'S')
// METHODONLY: method-throw-only.cpp:13:10: error: unsupported: call to unimported method 'S_get'

//--- ctor-throw.cpp
// A throw inside a CONSTRUCTOR stays rejected (wave-2 at the earliest: a
// throwing constructor has no half-constructed-object image).
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
class C {
public:
  int v;
  C(int x) {
    if (x)
      throw 7;
    v = x;
  }
};
int main() {
  C c(0);
  int ok = 0;
  try {
    ok = thrower(1);
  } catch (int e) {
    ok = e;
  }
  return c.v + ok;
}
// CTORTHROW: ctor-throw.cpp:13:7: error: unsupported: throw outside a try block in a function that cannot propagate exceptions

//--- noexcept-throw.cpp
// noexcept is the closure boundary: a throw reaching one is
// std::terminate, and the terminate helper is wave-2.
int boom(int x) noexcept {
  if (x == 0)
    throw 2;
  return x;
}
int main() { return boom(1); }
// NOEXCEPT: noexcept-throw.cpp:3:5: error: unsupported: a throw reaching a noexcept function (std::terminate has no image)

//--- two-payloads.cpp
// ONE thrown payload type per TU: one carrier enum threads every
// signature, so a second payload type has no representation.
int f(int x) {
  if (x == 0)
    throw 1;
  return x;
}
double g(double x) {
  if (x < 1.0)
    throw 2.5;
  return x;
}
int main() {
  int ok = 0;
  try {
    ok = f(1) + (int)g(2.0);
  } catch (int e) {
    ok = e;
  }
  return ok;
}
// TWOPAYLOADS: two-payloads.cpp:10:5: error: unsupported: more than one thrown exception payload type in a translation unit

//--- class-payload.cpp
// A class payload stays rejected: a struct RIDES the enum fine, but
// reading its field in a match arm has no op (emitrust.member needs an
// lvalue; a match arm binds an SSA value).
struct P {
  int a;
};
int f(int x) {
  if (x == 0)
    throw P();
  return x;
}
int main() { return f(1); }
// CLASSPAYLOAD: class-payload.cpp:9:5: error: unsupported: thrown exception payload must be a supported scalar type

//--- catch-by-ref.cpp
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
int main() {
  int ok = 0;
  try {
    ok = thrower(0);
  } catch (int &e) {
    ok = e;
  }
  return ok;
}
// CATCHBYREF: catch-by-ref.cpp:10:17: error: unsupported: catch by reference

//--- catch-mismatch.cpp
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
int main() {
  int ok = 0;
  try {
    ok = thrower(0);
  } catch (double e) {
    ok = (int)e;
  }
  return ok;
}
// CATCHMISMATCH: catch-mismatch.cpp:10:19: error: unsupported: catch of a type other than the thrown payload type

//--- rethrow-outside-catch.cpp
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
int main() {
  int ok = 0;
  try {
    ok = thrower(1);
  } catch (int e) {
    ok = e;
  }
  throw;
}
// RETHROWOUT: rethrow-outside-catch.cpp:13:3: error: unsupported: rethrow outside a catch handler

//--- address-taken.cpp
// An address-taken closure member is THE unresolvable-edge shape: a call
// through the pointer would bypass the signature rewrite entirely.
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
int main() {
  int (*fp)(int) = thrower;
  int ok = 0;
  try {
    ok = thrower(1);
  } catch (int e) {
    ok = e;
  }
  return fp(1) + ok;
}
// ADDRTAKEN: address-taken.cpp:9:20: error: unsupported: the address of a potentially-throwing function is taken

//--- variadic-throw.cpp
int vthrow(int x, ...) {
  if (x == 0)
    throw 1;
  return x;
}
int main() {
  int ok = 0;
  try {
    ok = vthrow(1);
  } catch (int e) {
    ok = e;
  }
  return ok;
}
// VARIADICTHROW: variadic-throw.cpp:1:5: error: unsupported: a potentially-throwing function with a variadic signature

//--- return-mismatch.cpp
// Wave-1 gate: Ok0 and Err0 share ONE payload type, so a closure member
// must return the thrown payload type (a single payload match serves both
// the value and the propagation — the byte-diff-verified unwrap shape).
double dthrow(int x) {
  if (x == 0)
    throw 1;
  return 1.5;
}
int main() {
  int ok = 0;
  try {
    ok = (int)dthrow(1);
  } catch (int e) {
    ok = e;
  }
  return ok;
}
// RETMISMATCH: return-mismatch.cpp:4:8: error: unsupported: a potentially-throwing function must return the thrown payload type

//--- pointer-param.cpp
// Wave-1 gate: arithmetic parameters only, so a closure member can never
// meet the pointer-decomposition planners (cursor/owner/cell machinery)
// whose call paths would bypass the carrier unwrap.
int pthrow(int *p) {
  if (*p == 0)
    throw 1;
  return *p;
}
int main() {
  int v = 1;
  int ok = 0;
  try {
    ok = pthrow(&v);
  } catch (int e) {
    ok = e;
  }
  return ok;
}
// PTRPARAM: pointer-param.cpp:4:17: error: unsupported: a potentially-throwing function with non-scalar parameters

//--- multi-handler.cpp
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
int main() {
  int ok = 0;
  try {
    ok = thrower(0);
  } catch (int e) {
    ok = e;
  } catch (...) {
    ok = -1;
  }
  return ok;
}
// MULTIHANDLER: multi-handler.cpp:8:3: error: unsupported: a try statement with more than one catch handler

//--- nested-try.cpp
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
int main() {
  int ok = 0;
  try {
    try {
      ok = thrower(0);
    } catch (int e) {
      ok = e;
    }
  } catch (int e) {
    ok = e + 1;
  }
  return ok;
}
// NESTEDTRY: nested-try.cpp:9:5: error: unsupported: a try statement nested inside another try or catch

//--- try-in-method.cpp
// Wave-1 gate: TU-level functions only. A self-contained try inside a
// method would work mechanically, but it is unverified surface; FR-112's
// containment turns the rejection into the omission warning and the call
// site stays the hard error.
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
class M {
public:
  int shielded(int x) {
    int r = 0;
    try {
      r = thrower(x);
    } catch (int e) {
      r = e;
    }
    return r;
  }
};
int main() {
  M m;
  int ok = 0;
  try {
    ok = thrower(1);
  } catch (int e) {
    ok = e;
  }
  return m.shielded(1) + ok;
}
// TRYINMETHOD: try-in-method.cpp:14:5: warning: unsupported: try/catch outside a translation-unit-level function (omitted: method 'shielded' of class 'M')
// TRYINMETHOD: try-in-method.cpp:30:10: error: unsupported: call to unimported method 'M_shielded'

//--- droppy-in-try.cpp
// W2.17 divergence gate: C++ destroys a try-block local at try exit
// (BEFORE the handler runs); the image's places are function-scoped after
// structurization, so an observable destructor would fire at function
// exit instead. Rejected rather than allowed to diverge.
extern "C" int printf(const char *, ...);
int thrower(int x) {
  if (x == 0)
    throw 1;
  return x;
}
class D {
public:
  int tag;
  D(int t) { tag = t; }
  ~D() { printf("dtor %d\n", tag); }
};
int main() {
  int ok = 0;
  try {
    D d(1);
    ok = thrower(0);
  } catch (int e) {
    ok = e;
  }
  return ok;
}
// DROPPYINTRY: droppy-in-try.cpp:20:7: error: unsupported: a class with a destructor declared inside a try statement

//--- try-no-throw.cpp
// The INACTIVE-plan control for try: a TU with no throw at all never
// activates the plan, and a try keeps the historical generic statement
// fallback — pinned so the plan gate can never widen silently.
int main() {
  int x = 0;
  try {
    x = 1;
  } catch (int e) {
    x = e;
  }
  return x;
}
// TRYNOTHROW: try-no-throw.cpp:6:3: error: unsupported statement: CXXTryStmt
