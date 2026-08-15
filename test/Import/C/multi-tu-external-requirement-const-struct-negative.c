// FR-79/FR-80 boundaries: the const-struct requirement admits ONLY the
// shapes a trait item can faithfully express. FR-79 admitted the by-value
// read; FR-80 moved the pin forward for the ADDRESS-AT-ARGUMENT shape
// (`take(&cfg)` with a const-struct-pointee requirement parameter — now
// positive, see multi-tu-external-requirement-const-struct-addr.c) and this
// file pins the frontier that REMAINS rejected:
//   * a RETURNED address — a function return type has no `&'static`
//     spelling in the model (a bare `&S` return is rustc E0106);
//   * a pointer-identity COMPARE against the address — the pointer plan's
//     same-object model has no object identity for a requirement address;
//   * the address STORED into a struct field — the i64-cursor field model
//     cannot carry it;
//   * the NON-const struct extern's address — a mutable requirement
//     address would need `&'static mut`, which the dyn-free/unsafe-free
//     FR-52 contract excludes;
//   * an ARRAY of const structs — element access needs a place-yielding
//     projection the trait cannot express.
// The address-taken arms reject EARLIER than finalizeProject, with the
// pointer plan's use-shape-dependent wordings. FR-70's whole-program
// addressTakenGlobals gate is now SHAPE-AWARE (const structs may qualify
// with load/global_addr uses) but remains the backstop for every
// non-qualifying shape a recovery mode swallows.
// RUN: split-file %s %t
// RUN: not emitrust-import-c --externals-trait %t/addr-ret.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ADDR-RET %s
// RUN: not emitrust-import-c --externals-trait %t/addr-cmp.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ADDR-CMP %s
// RUN: not emitrust-import-c --externals-trait %t/addr-store.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ADDR-STORE %s
// RUN: not emitrust-import-c --externals-trait %t/addr-nonconst.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ADDR-NONCONST %s
// RUN: not emitrust-import-c --externals-trait %t/array.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ARRAY %s

//--- empty.c
int unrelated(int v) { return v; }

//--- addr-ret.c
// Returning the address of the const global: the &'static item spelling is
// confined to trait-item results; a project function's own return type
// keeps the pointer-return classification's rejection.
struct S {
  int x;
  int y;
};
extern const struct S cfg;

const struct S *loc(void) { return &cfg; }

int first(void) { return cfg.x; }
// ADDR-RET: addr-ret.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned address of a const global

//--- addr-cmp.c
// Comparing a pointer against the global's address: the same-object cursor
// model compares (base, cursor) facts, and a requirement address has no
// VarDecl base to compare — identity lives behind the consumer's item.
struct S {
  int x;
  int y;
};
extern const struct S cfg;

int same(const struct S *s) { return s == &cfg; }

int first(void) { return cfg.x; }
// ADDR-CMP: addr-cmp.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: comparison of pointers into different objects

//--- addr-store.c
// Storing the address into a struct field: a data-pointer field is an i64
// cursor fact, and a requirement address has no cursor into any modeled
// region — the static-binding rejection stands, located at the store.
struct S {
  int x;
  int y;
};
struct Holder {
  const struct S *ptr;
};
extern const struct S cfg;

void keep(struct Holder *h) { h->ptr = &cfg; }

int first(void) { return cfg.x; }
// ADDR-STORE: addr-store.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member 'ptr' is used outside the static-binding model

//--- addr-nonconst.c
// The NON-const struct extern's address at the same argument position that
// FR-80 flipped for const: a mutable requirement address would need
// `&'static mut`, so the parameter stays a mutable borrow and the address
// keeps the pointer-plan rejection.
struct S {
  int x;
  int y;
};
extern struct S cfg;
int take(const struct S *p);

int use(void) { return take(&cfg); }

int first(void) { return cfg.x; }
// ADDR-NONCONST: addr-nonconst.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function

//--- array.c
// An ARRAY of const structs keeps the verbatim finalize rejection: element
// access needs a place-yielding projection into environment-owned storage,
// which no trait item yields, and the frontier stays decided on the TYPE,
// not on which accesses an optimization happened to leave.
struct S {
  int x;
  int y;
};
extern const struct S table[4];

int first(void) { return table[0].x; }
// ARRAY: array.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'table' is referenced but not defined in any translation unit
