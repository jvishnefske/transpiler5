// Companion translation unit for test/EndToEnd/cpp-rejected-class-no-trace.cpp.
// Neither class here is ever instantiated by the program, so `clang++` runs
// NEITHER destructor; both classes are rejected by the importer, one at the
// class-level constructor gate and one from inside a DESTRUCTOR body, and
// on unpatched (pre-FR-118) HEAD both left a `struct_def` (carrying
// `emitrust.has_drop`) behind for the other TU's PODs to merge with.
//
// FR-112 RESHAPED the second class: an ORDINARY method's body failure is
// now contained (the method is omitted and the class imports), so the
// original `int bad()` shape would legitimately claim the emitted name `D`
// -- with `emitrust.has_drop` -- and this test's POD `struct d` would merge
// onto a LIVE drop-carrying class, which is exactly the divergence the test
// exists to forbid. The failure moved into the DESTRUCTOR body, the one
// in-`importCXXMethods` body failure that CANNOT be contained (a destructor
// runs implicitly at scope exit; there is no call node to reject), so the
// class-level undo -- and this byte-diff's original premise -- still has a
// live body-failure channel to pin.
extern "C" int printf(const char *, ...);

// Class-level gate: a MOVE constructor (the pre-W2.23 copy-ctor specimen
// is admitted now; the gate and its wording are unchanged). Rejected inside
// `importCXXMethods`, after the struct_def is already in the module.
struct C {
  int v;
  C() : v(1) {}
  C(C &&o) : v(2) {}
  ~C();
};

C::~C() { printf("lib dtor C %d\n", v); }

// Body-level failure that survives FR-112 containment: an unsupported
// pointer expression inside the DESTRUCTOR. No class-level predicate can
// hoist this one, which is why the fix had to be the undo rather than a
// check moved earlier.
struct D {
  int w;
  D() : w(1) {}
  ~D() { printf("lib dtor D %d\n", *(int *)(long)w); }
};
