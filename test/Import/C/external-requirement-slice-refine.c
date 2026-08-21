// FR-75 classification timing: under a trait policy a body-less data-
// pointer parameter classifies as a SLICE eagerly (the requirement-shape
// assumption), so the cross-TU decl-first ordering changes in BOTH
// directions, each located, never silent:
//
//  * IMPROVEMENT: decl-first + a later defining TU whose body walks the
//    parameter used to be the "was called ... before its definition
//    refined the signature" rejection (pointers-param-invalid.c CROSSTU
//    pins the bin-mode wording, which is unchanged); under the trait
//    policy the eager slice makes the two signatures EQUAL and the
//    project imports.
//  * DELIBERATE, BOUNDED REGRESSION (recorded in the FR-75 entry): decl-
//    first + a later defining TU whose body only DEREFERENCES the
//    parameter now conflicts (slice vs scalar) and keeps the located
//    conflicting-redeclaration rejection.
//
// RUN: split-file %s %t
// RUN: emitrust-import-c --externals-trait %t/decl-first.c %t/def-slice.c \
// RUN:   | FileCheck %s
//
// The definition satisfies the eager slice signature: one defined
// function, no requirement marker anywhere.
// CHECK: func.func @helper(%{{.*}}: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{.*}}: ui32)
// CHECK-NOT: external_requirement
//
// RUN: not emitrust-import-c --externals-trait %t/decl-first.c \
// RUN:   %t/def-scalar.c 2>&1 | FileCheck --check-prefix=SCALARDEF %s
// SCALARDEF: def-scalar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting redeclaration of 'helper'
//
// FR-100 interaction (the seeding constraint): a defining TU whose body
// only FORWARDS the parameter on to a static helper. FR-100's TU-wide
// forwarding fixpoint would otherwise classify `helper`'s parameter
// from the forwarding shape alone and DISAGREE with the eager slice the
// decl-only TU already assumed, i.e. this project would stop importing
// with "conflicting redeclaration of 'helper'". The fixpoint is
// therefore ALSO seeded with the eager requirement classification for
// an externally visible function in a trait-eligible TU, and because it
// is a SEED (not a lookup special-case) the demand propagates BACKWARD
// through the forwarding edge: the static CALLER `outer`, which forwards
// into `helper`, is demoted to a slice with it instead of mismatching at
// the call. `inner` (the callee helper forwards INTO, which only
// dereferences) keeps its scalar reference and is reached through the
// existing element reborrow, which is what makes the seed sound rather
// than a blanket demotion.
// RUN: emitrust-import-c --externals-trait %t/decl-first.c %t/def-fwd.c \
// RUN:   | FileCheck --check-prefix=DEFFWD %s
// DEFFWD-DAG: func.func @helper(%{{.*}}: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{.*}}: ui32)
// DEFFWD-DAG: func.func @tu1_outer(%{{.*}}: !emitrust.mut_ref<!emitrust.slice<ui8>>, %{{.*}}: ui32)
// DEFFWD-DAG: func.func @tu1_inner(%{{.*}}: !emitrust.mut_ref<ui8>, %{{.*}}: ui32)

//--- decl-first.c
void helper(unsigned char *buf, unsigned int len);

int use_it(void) {
  unsigned char b[4] = {1, 2, 3, 4};
  helper(b, 4u);
  return (int)b[2];
}

//--- def-slice.c
void helper(unsigned char *buf, unsigned int len) {
  unsigned int i;
  for (i = 0; i < len; i++)
    buf[i] = (unsigned char)(buf[i] + 1u);
}

//--- def-scalar.c
void helper(unsigned char *buf, unsigned int len) {
  *buf = (unsigned char)len;
}

//--- def-fwd.c
static void inner(unsigned char *p, unsigned int n) { *p = (unsigned char)n; }

void helper(unsigned char *buf, unsigned int len) { inner(buf, len); }

static void outer(unsigned char *q, unsigned int n) { helper(q, n); }

void kick(unsigned char *r, unsigned int n) { outer(r, n); }
