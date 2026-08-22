// Companion translation unit for test/EndToEnd/cpp-rejected-class-no-trace.cpp.
// Neither class here is ever instantiated by the program, so `clang++` runs
// NEITHER destructor; both classes are rejected by the importer, one at the
// class level and one from inside a method body, and on unpatched HEAD both
// left a `struct_def` (carrying `emitrust.has_drop`) behind for the other
// TU's PODs to merge with.
extern "C" int printf(const char *, ...);

// Class-level gate: a copy constructor. Rejected inside `importCXXMethods`,
// after the struct_def is already in the module.
struct C {
  int v;
  C() : v(1) {}
  C(const C &o) : v(2) {}
  ~C();
};

C::~C() { printf("lib dtor C %d\n", v); }

// Body-level failure: an unsupported pointer expression inside an ordinary
// method. No class-level predicate can hoist this one, which is why the fix
// had to be the undo rather than a check moved earlier.
struct D {
  int w;
  D() : w(1) {}
  int bad() const { return *(int *)(long)w; }
  ~D();
};

D::~D() { printf("lib dtor D %d\n", w); }
