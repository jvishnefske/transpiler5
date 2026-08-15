// FR-78 negative space: a differing-aggregate-arm union imports as OPAQUE
// STORAGE (union-opaque-aggregate.c), so the TYPE and every whole-value
// use are admitted — but no access path through ANY arm is representable
// on the blob, and each one must die as a LOCATED rejection at its own
// access site, never as leaked Rust (the pre-FR-78 placeholder broke 42
// crates with rustc E0609 because its guard missed access paths; this
// file enumerates them). Pinned per shape: arm reads, arm writes, nested
// projection, compound assignment, ++, arrow access through a
// pointer-to-union parameter (all `unsupported: opaque union arm
// access`), active-arm initializers — local designated, non-zero global
// constant, compound literal (`unsupported: opaque union arm
// initializer`), address-of an arm (the existing union-member-address
// rejection), and the CTS-R2 anonymous-MEMBER union, which keeps its
// record-level `unsupported: union type` rejection (the opaque model is
// for named/typed unions only).
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/read.c 2>&1 | FileCheck %s --check-prefix=READ
// RUN: not emitrust-import-c %t/write.c 2>&1 | FileCheck %s --check-prefix=WRITE
// RUN: not emitrust-import-c %t/nested.c 2>&1 | FileCheck %s --check-prefix=NESTED
// RUN: not emitrust-import-c %t/compound.c 2>&1 | FileCheck %s --check-prefix=COMPOUND
// RUN: not emitrust-import-c %t/incdec.c 2>&1 | FileCheck %s --check-prefix=INCDEC
// RUN: not emitrust-import-c %t/arrow.c 2>&1 | FileCheck %s --check-prefix=ARROW
// RUN: not emitrust-import-c %t/addrof.c 2>&1 | FileCheck %s --check-prefix=ADDROF
// RUN: not emitrust-import-c %t/init-local.c 2>&1 | FileCheck %s --check-prefix=INITLOCAL
// RUN: not emitrust-import-c %t/init-global.c 2>&1 | FileCheck %s --check-prefix=INITGLOBAL
// RUN: not emitrust-import-c %t/compound-literal.c 2>&1 | FileCheck %s --check-prefix=CLIT
// RUN: not emitrust-import-c %t/anon-member.c 2>&1 | FileCheck %s --check-prefix=ANONMEMBER

// READ: read.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm access
// WRITE: write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm access
// NESTED: nested.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm access
// COMPOUND: compound.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm access
// INCDEC: incdec.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm access
// ARROW: arrow.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm access
// ADDROF: addrof.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of a union member
// INITLOCAL: init-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm initializer
// INITGLOBAL: init-global.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm initializer
// CLIT: compound-literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm initializer
// ANONMEMBER: anon-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union type

//--- read.c
// A plain read through an arm: the blob carries no arm-typed view.
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

int read_arm(void) {
  union U u;
  return u.b.bs;
}

//--- write.c
// A write through an arm (dot access on a local).
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

void write_arm(void) {
  union U u;
  u.b.bs = 1;
}

//--- nested.c
// Deep projection through an arm (u.a.ax[3]) dies at the ARM selection,
// not somewhere down the chain.
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

struct Rec {
  int before;
  union U u;
  int after;
};

int deep(void) {
  struct Rec r;
  return r.u.a.ax[3];
}

//--- compound.c
// Compound assignment through an arm funnels into the same lvalue path.
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

void comp(void) {
  union U u;
  u.b.bs += 2;
}

//--- incdec.c
// ++ through an arm funnels into the same lvalue path.
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

void bump(void) {
  union U u;
  u.b.bs++;
}

//--- arrow.c
// Arrow access through a pointer-to-union parameter: the same arm
// interception must hold when the union arrives by reference.
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

int through_ptr(union U *p) { return p->a.ax[0]; }

//--- addrof.c
// &u.a: taking the address of a union member keeps its established
// located rejection (the opaque model adds no new address path).
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

int addr_of(void) {
  union U u;
  struct A *p = &u.a;
  return p->ax[0];
}

//--- init-local.c
// A designated LOCAL initializer naming an arm: the historical leak path
// (the braced union initializer emitted the arm's member op with no
// guard), now a located rejection at the initializer.
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

int init_local(void) {
  union U u = {.b = {1, 2}};
  return 0;
}

//--- init-global.c
// A NON-zero global arm constant cannot land on the blob this wave (only
// the all-zero static zero-fill is admitted, see
// union-opaque-aggregate.c global-zero).
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

union U g = {.b = {1, 2}};

//--- compound-literal.c
// A compound literal naming an arm is the same initializer path.
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

union U {
  struct A a;
  struct B b;
};

int clit(void) {
  union U u = (union U){.b = {1, 2}};
  return 0;
}

//--- anon-member.c
// An anonymous-MEMBER union (unnamed field) with differing aggregate arms
// keeps the CTS-R2 record-level rejection: its arms flatten into the
// PARENT struct_def, so there is no union struct_def to make opaque.
struct A {
  int ax[10];
};

struct B {
  short bs;
  char bc;
};

struct S {
  int tag;
  union {
    struct A a;
    struct B b;
  };
};

struct S g;
