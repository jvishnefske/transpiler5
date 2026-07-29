// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/ref-return.cpp 2>&1 | FileCheck %s --check-prefix=REFRETURN
// RUN: not emitrust-import-c %t/ref-member.cpp 2>&1 | FileCheck %s --check-prefix=REFMEMBER
// RUN: not emitrust-import-c %t/rvalue-ref.cpp 2>&1 | FileCheck %s --check-prefix=RVALUEREF
// RUN: not emitrust-import-c %t/ref-to-pointer.cpp 2>&1 | FileCheck %s --check-prefix=REFTOPTR
// RUN: not emitrust-import-c %t/ref-to-array.cpp 2>&1 | FileCheck %s --check-prefix=REFTOARRAY
// RUN: not emitrust-import-c %t/ref-local.cpp 2>&1 | FileCheck %s --check-prefix=REFLOCAL

// FR-48 located-rejection ledger: the reference positions that stay OUT of
// scope now that the PARAMETER position is in (test/Import/Cpp/
// cpp-references.cpp). Before FR-48 every one of these six produced the
// single undifferentiated wording "reference types are not yet supported";
// five of them now carry a wording naming the specific position, which is
// the point of this file — a reader who hits one of these should learn
// which reference position is missing, not merely that references in
// general are.
//
// The sixth (a reference LOCAL) deliberately KEEPS the historical wording.
// It is the residual case: whatever position a `T&` turns up in that FR-48
// did not enumerate falls through to it, so specializing it would leave the
// genuinely-unclassified positions with no diagnostic at all. Its presence
// here is a pin that the fallback still fires and still says what it always
// said.
//
// Every case is rejected at the DECLARATION that introduces the reference,
// not at a use — the column in each check is the reference declarator's own
// — so the rejection does not depend on the construct ever being reached at
// run time. Each case is nonetheless written so its reference is actually
// used, to keep the case honest if a future wave accepts the declaration
// and has to reject something later instead.

//--- ref-return.cpp
// A reference RETURN is out of scope: it hands the caller a borrow whose
// lifetime is tied to something the signature does not name, which is a
// lifetime-annotation problem (Rust would need an explicit `'a` relating
// the result to a parameter) and not a type-mapping one. Rejected at the
// function declaration, before the reference PARAMETER — which FR-48 does
// accept — is even considered.
// REFRETURN: ref-return.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference return types are not yet supported
int &pick(int &a) { return a; }

int use(void) {
  int v = 1;
  return pick(v);
}

//--- ref-member.cpp
// A reference-typed data MEMBER is out of scope for the same lifetime
// reason as the return case, plus a construction one: a struct holding a
// borrow is `struct Holder<'a> { r: &'a i32 }` in Rust, so the lifetime
// infects the type name and every use of it. This case predates FR-48 (it
// was pinned as methods-invalid.cpp's REFMEMBER against the generic
// wording); FR-48 gives it its own wording without changing where or when
// it fires.
// REFMEMBER: ref-member.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference struct members are not yet supported
int global_val = 3;

struct Holder {
  int &r;
  int get() const { return r; }
};

int use(void) {
  Holder h{global_val};
  return h.get();
}

//--- rvalue-ref.cpp
// An rvalue reference `T&&` is out of scope ANYWHERE, parameter position
// included. It is not a borrow at all in the Rust sense: binding one is an
// ownership transfer, and the useful C++ idioms built on it (move
// construction, perfect forwarding) need move semantics and reference
// collapsing that this importer has no model for. Mapping it to
// `mut_ref` the way FR-48 maps `T&` would be actively wrong, so it is
// rejected rather than approximated.
// RVALUEREF: rvalue-ref.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: rvalue reference types are not yet supported
int consume(int &&v) { return v + 1; }

int use(void) {
  return consume(41);
}

//--- ref-to-pointer.cpp
// A reference to a DATA POINTER (`int *&`) is out of scope even though it
// is a plain lvalue reference in an accepted position. The referent is a
// pointer the callee may reseat, and this importer does not represent a
// data pointer as a single first-class SSA value that a borrow could point
// at — pointer locals are decomposed into a base place plus cursor/
// non-null cells (the `pointerLocals` machinery). There is no `&mut ptr`
// to hand out, so the parameter is rejected at its own declarator.
// REFTOPTR: ref-to-pointer.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference-to-pointer parameter
int deref_through(int *&p) { return *p; }

int use(void) {
  int v = 41;
  int *p = &v;
  return deref_through(p);
}

//--- ref-to-array.cpp
// A reference to an ARRAY (`int (&a)[4]`) is out of scope: the natural
// Rust spelling is a slice (`&[i32]`) or a fixed-size array borrow
// (`&[i32; 4]`), and the choice between them is a policy this wave does not
// settle. Rejected at the parameter declarator rather than silently
// decaying to the pointer parameter the equivalent C signature would use,
// because the decay would throw away the length the C++ type carries.
// REFTOARRAY: ref-to-array.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference-to-array parameter
int first(int (&a)[4]) { return a[0]; }

int use(void) {
  int buf[4];
  buf[0] = 41;
  return first(buf);
}

//--- ref-local.cpp
// A reference LOCAL is the residual case, and it keeps the pre-FR-48
// wording on purpose (see this file's header). It is genuinely harder than
// a parameter: a parameter's borrow is created by the CALLER and lives for
// the whole call, whereas a local alias must be created, scoped and
// invalidated within one body, interleaved with every other use of the
// place it names — which is exactly the analysis the importer does not do.
// This wording is also what any reference position FR-48 did not enumerate
// (a reference global, for instance) still reports.
// REFLOCAL: ref-local.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference types are not yet supported
int use(void) {
  int v = 41;
  int &r = v;
  r = r + 1;
  return v;
}
