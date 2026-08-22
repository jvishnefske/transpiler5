// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/multiple.cpp 2>&1 | FileCheck %s --check-prefix=MULTIPLE
// RUN: not emitrust-import-c %t/virtual-base.cpp 2>&1 | FileCheck %s --check-prefix=VIRTBASE
// RUN: not emitrust-import-c %t/private-base.cpp 2>&1 | FileCheck %s --check-prefix=PRIVBASE
// RUN: not emitrust-import-c %t/protected-base.cpp 2>&1 | FileCheck %s --check-prefix=PROTBASE
// RUN: not emitrust-import-c %t/template-base.cpp 2>&1 | FileCheck %s --check-prefix=TMPLBASE
// RUN: not emitrust-import-c %t/virtual-method-base.cpp 2>&1 | FileCheck %s --check-prefix=VIRTMETHOD
// RUN: not emitrust-import-c %t/base-field-collision.cpp 2>&1 | FileCheck %s --check-prefix=COLLIDE
// RUN: not emitrust-import-c %t/empty-base-call.cpp 2>&1 | FileCheck %s --check-prefix=EMPTYCALL
// RUN: not emitrust-import-c %t/empty-base-ctor.cpp 2>&1 | FileCheck %s --check-prefix=EMPTYCTOR
// RUN: not emitrust-import-c %t/slice-argument.cpp 2>&1 | FileCheck %s --check-prefix=SLICEARG
// RUN: not emitrust-import-c %t/slice-copy-init.cpp 2>&1 | FileCheck %s --check-prefix=SLICECOPY
// RUN: not emitrust-import-c %t/upcast-reference.cpp 2>&1 | FileCheck %s --check-prefix=UPREF

// W2.18 located-rejection ledger for single non-virtual inheritance. The
// wave admitted exactly ONE shape -- a single PUBLIC, NON-VIRTUAL base,
// defined in this translation unit, that is not a class-template
// specialization, accessed only through a derived OBJECT place or the
// derived class's own `this` (see inheritance.cpp); W2.26 extended it to
// a base CARRYING a destructor (inheritance-drop.cpp). That subset was chosen because it is the subset that
// byte-diffs clean against `clang++ -std=c++17`; every shape below either
// has no base-as-first-field image at all, or was MEASURED to diverge.
//
// The measured reason, one per case:
//
// * multiple inheritance: two bases need two fields, and the single `base`
//   name (with every inherited access flattened through it) stops being
//   unambiguous. Keeps the W2.0 wording.
// * a VIRTUAL base: one shared subobject reached from several derived
//   paths, which a by-value field cannot represent -- each path would get
//   its own copy. Keeps the W2.0 wording.
// * PRIVATE/PROTECTED inheritance: "implemented in terms of", so the
//   base's members are not part of the derived interface; a plain `base`
//   field makes every one of them reachable from anywhere in the crate.
//   Both keep the W2.0 wording.
// * a class-template SPECIALIZATION as the base: W2.16 monomorphizes a
//   class template at its point of use, and a specialization reached only
//   as a base has never been exercised through that ordering. Keeps the
//   W2.0 wording. (The reverse -- a class TEMPLATE as the DERIVED class --
//   is admitted; it is pinned in class-templates.cpp.)
// * a base carrying a USER-DECLARED DESTRUCTOR: ADMITTED since W2.26.
//   The E0204/lost-drop channel that kept it out is closed by the
//   TRANSITIVE drop predicate (a merely-inheriting class now answers
//   droppy, so every W2.17 use-site gate fires for it and the emitter
//   suppresses `Copy`); the pin moved FORWARD to
//   test/Import/Cpp/inheritance-drop.cpp and the byte-diff oracle
//   test/EndToEnd/cpp-inheritance-drop.cpp. The residual drop-family
//   rejections around the admitted chain (droppy member BESIDE a droppy
//   base, arrays/globals/by-value/copies/bare-block objects of a
//   droppy-DERIVED class) are pinned in inheritance-drop-invalid.cpp.
// * any VIRTUAL METHOD in the chain: no vtable, no dynamic dispatch. Not
//   a new rejection -- the base class's own import raises it first, at the
//   method, which is where the real blocker is. W2.19's job.
// * a derived member literally spelled `base`: it would collide with the
//   synthesized field. Caught by `appendField`'s pre-existing duplicate-
//   final-name guard, which works ONLY because the base field is
//   PREPENDED before the `fields()` walk.
// * an EMPTY base: it contributes no field (a `[u8; 1]` placeholder would
//   be invention, and C++ applies the empty-base optimization), so there
//   is no `base` to project through. An inherited METHOD call and a base
//   CONSTRUCTOR with a body both reject rather than emitting a field
//   access that does not exist (rustc E0609) or dropping the constructor's
//   side effects in silence. A TRIVIAL empty base construction is elided,
//   because it has no observable effect at all.
// * SLICING -- passing a Derived BY VALUE where a Base is expected, and
//   copy-initializing a Base from a Derived. C++ copies only the base
//   subobject; the base-as-first-field image has no whole-object move that
//   reproduces that. This is the wave's #1 miscompile channel: the
//   trivial-copy unwrap in `emitRValue`'s CXXConstructExpr arm would have
//   moved the ENTIRE Derived. Deliberately NOT given a `DerivedToBase`
//   case in `emitCast` -- that one line is what would turn this located
//   rejection into a silent whole-object move.
// * the UPCAST REFERENCE (`Base &r = d;`): still blocked by the
//   pre-existing reference rejection. The UPCAST POINTER (`Base *p =
//   &d;`) and the inherited member through a DERIVED-class pointer were
//   pinned here as UPPTR/DERIVEDPTR until FR-120 flipped both to
//   positives: the pins moved FORWARD into inheritance-upcast.cpp and
//   the byte-diff oracle test/EndToEnd/cpp-upcast-pointer.cpp; the
//   residual pointer fences (multi-object regions, joins, globals,
//   base-pointer params/members/arrays) live in
//   struct-pointer-methods-invalid.cpp, and the polymorphic-chain fence
//   in inheritance-drop-invalid.cpp's VUPCAST arm.
//
// An UNDEFINED base needs no importer wording and has none: clang
// hard-errors `base class has incomplete type` before the importer ever
// runs. Measured, and recorded here as a non-requirement rather than as a
// wording that could never fire.

//--- multiple.cpp
struct A { int a; };
struct B { int b; };
// MULTIPLE: multiple.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: base classes are not supported
struct D : A, B { int c; };
int use() { D d; d.c = 1; return d.c; }

//--- virtual-base.cpp
struct A { int a; };
// VIRTBASE: virtual-base.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: base classes are not supported
struct D : virtual A { int c; };
int use() { D d; d.c = 1; return d.c; }

//--- private-base.cpp
struct A { int a; };
// PRIVBASE: private-base.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: base classes are not supported
struct D : private A { int c; };
int use() { D d; d.c = 1; return d.c; }

//--- protected-base.cpp
struct A { int a; };
// PROTBASE: protected-base.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: base classes are not supported
struct D : protected A { int c; };
int use() { D d; d.c = 1; return d.c; }

//--- template-base.cpp
template <typename T> struct Box { T v; };
// TMPLBASE: template-base.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: base classes are not supported
struct D : Box<int> { int c; };
int use() { D d; d.c = 1; return d.c; }

//--- virtual-method-base.cpp
// The base's own import raises this, at the method, before the derived
// class's base loop is ever reached.
struct A {
  int a;
  // VIRTMETHOD: virtual-method-base.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: virtual method
  virtual int get() const { return a; }
};
struct D : A { int c; };
int use() { D d; d.c = 1; return d.c; }

//--- base-field-collision.cpp
struct A { int a; };
struct D : A {
  // COLLIDE: base-field-collision.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct member 'base' maps to the Rust field name 'base', which collides with another member
  int base;
};
int use() { D d; d.base = 1; return d.base; }

//--- empty-base-call.cpp
struct Tag { int id() const { return 1; } };
struct D : Tag { int c; };
// EMPTYCALL: empty-base-call.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: inherited member of an empty base class
int use() {
  D d;
  d.c = 1;
  return d.id() + d.c;
}

//--- empty-base-ctor.cpp
extern "C" int printf(const char *, ...);
struct Tag { Tag() { printf("tag\n"); } };
struct D : Tag {
  int c;
  // EMPTYCTOR: empty-base-ctor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: constructor of an empty base class
  D(int v) : Tag(), c(v) {}
};
int use() { D d(1); return d.c; }

//--- slice-argument.cpp
struct A {
  int a;
  A(int v) : a(v) {}
  int get() const { return a; }
};
struct D : A {
  int c;
  D(int v) : A(v), c(v) {}
};
int takesBase(A b) { return b.get(); }
// SLICEARG: slice-argument.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported cast (DerivedToBase)
int use() {
  D d(1);
  return takesBase(d);
}

//--- slice-copy-init.cpp
struct A {
  int a;
  A(int v) : a(v) {}
  int get() const { return a; }
};
struct D : A {
  int c;
  D(int v) : A(v), c(v) {}
};
// SLICECOPY: slice-copy-init.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: copy/move construction
int use() {
  D d(1);
  A b = d;
  return b.get();
}

//--- upcast-reference.cpp
struct A {
  int a;
  A(int v) : a(v) {}
  int get() const { return a; }
};
struct D : A {
  int c;
  D(int v) : A(v), c(v) {}
};
// UPREF: upcast-reference.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference types are not yet supported
int use() {
  D d(1);
  A &r = d;
  return r.get();
}
