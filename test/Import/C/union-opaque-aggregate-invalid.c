// FR-78/FR-83 negative space: a differing-aggregate-arm union imports as
// OPAQUE STORAGE (union-opaque-aggregate.c), and FR-83 opened exactly ONE
// access family on the blob — integer-SCALAR leaves (including
// array-element leaves), pinned positive in union-opaque-arm-access.c.
// Everything else through an arm is still not representable and must die
// as a LOCATED rejection at its own access site, never as leaked Rust
// (the pre-FR-78 placeholder broke 42 crates with rustc E0609 because its
// guard missed access paths; this file enumerates the surviving
// frontier). Pinned per shape: whole-ARM writes and FLOAT leaves (the
// scalar-leaf model is IntegerType-only) keep `unsupported: opaque union
// arm access`; a whole-ARM aggregate read keeps its generic
// expression rejection; an arm ARRAY decaying to a pointer keeps the
// pointer-cast rejection; active-arm initializers — local designated,
// non-zero global constant, compound literal — keep `unsupported: opaque
// union arm initializer`; address-of an arm keeps the existing
// union-member-address rejection (NOT probed in return position, where
// the returned-pointer rejection preempts it); and — since FR-167 routed
// the non-flattening ANONYMOUS-member union through the same opaque
// import as the named one — an arm reached through the IMPLICIT
// anonymous hop lands on exactly the same access-site rejections as the
// named spelling, instead of the record dying at `unsupported: union
// type` before any access is ever analyzed. That last leg is the one
// that proves FR-167's synthesized-member PROJECTION is in place: if the
// implicit hop still peeled transparently, the arm write would select
// `opaque` on the PARENT struct and surface as FR-173's emitter backstop
// ("member 'opaque' does not exist on struct 'S'") rather than as a
// located importer rejection.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/whole-arm-read.c 2>&1 | FileCheck %s --check-prefix=WHOLEARMREAD
// RUN: not emitrust-import-c %t/whole-arm-write.c 2>&1 | FileCheck %s --check-prefix=WHOLEARMWRITE
// RUN: not emitrust-import-c %t/array-decay.c 2>&1 | FileCheck %s --check-prefix=ARRAYDECAY
// RUN: not emitrust-import-c %t/float-leaf.c 2>&1 | FileCheck %s --check-prefix=FLOATLEAF
// RUN: not emitrust-import-c %t/addrof.c 2>&1 | FileCheck %s --check-prefix=ADDROF
// RUN: not emitrust-import-c %t/init-local.c 2>&1 | FileCheck %s --check-prefix=INITLOCAL
// RUN: not emitrust-import-c %t/init-global.c 2>&1 | FileCheck %s --check-prefix=INITGLOBAL
// RUN: not emitrust-import-c %t/compound-literal.c 2>&1 | FileCheck %s --check-prefix=CLIT
// RUN: not emitrust-import-c %t/anon-member.c 2>&1 | FileCheck %s --check-prefix=ANONMEMBER

// WHOLEARMREAD: whole-arm-read.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported expression: MemberExpr
// WHOLEARMWRITE: whole-arm-write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm access
// ARRAYDECAY: array-decay.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)
// FLOATLEAF: float-leaf.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm access
// ADDROF: addrof.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of a union member
// INITLOCAL: init-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm initializer
// INITGLOBAL: init-global.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm initializer
// CLIT: compound-literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm initializer
// ANONMEMBER: anon-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm access
// ANONMEMBER-NOT: does not exist on struct

//--- whole-arm-read.c
// A whole-ARM aggregate copy out of the union: the blob has no arm-typed
// value to load, and the lwIP demand for whole-arm shapes is pointer
// comparison / const calls / escapes, not leaf access — out of the FR-83
// scalar-leaf scope.
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

int whole_arm_read(void) {
  union U u;
  struct B b2 = u.b;
  return b2.bs;
}

//--- whole-arm-write.c
// A whole-ARM aggregate store into the union dies at the arm selection.
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

void whole_arm_write(union U *u, struct B b2) { u->b = b2; }

//--- array-decay.c
// An arm ARRAY decaying to a pointer argument: no blob byte view yields a
// pointer, so the decay keeps its pointer-cast rejection.
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

void sink(int *p);

void decay(void) {
  union U u;
  sink(u.a.ax);
}

//--- float-leaf.c
// A FLOAT leaf through an arm: the byte-view image is IntegerType-only
// (ne_bytes helpers are emitted for integer widths), so a float leaf
// keeps the arm-access rejection at its own site.
union F {
  struct FA {
    float f;
  } fa;
  struct FB {
    int i;
  } ib;
};

float float_leaf(union F *p) { return p->fa.f; }

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
// An anonymous-MEMBER union (unnamed field) with differing aggregate
// arms: the RECORD imports since FR-167 (the union becomes its own
// opaque struct_def under a synthesized parent field), so the frontier
// moves to the access site — a whole-ARM aggregate store through the
// implicit anonymous hop keeps the same located rejection the named
// spelling gets.
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

void anon_whole_arm_write(struct S *s, struct B b2) { s->b = b2; }
