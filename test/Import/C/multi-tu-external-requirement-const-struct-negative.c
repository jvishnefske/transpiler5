// FR-79 boundaries: the const-struct requirement admits ONLY the shape the
// getter can faithfully express -- a whole-value read of a const,
// never-address-taken, struct-typed extern. Everything else keeps rejecting.
// The NON-const struct extern's verbatim rejection is already pinned by the
// GLOBAL-AGG arm of multi-tu-external-requirement-negative.c and stays
// byte-unchanged; this file pins the remaining frontier. NOTE the
// address-taken arms reject EARLIER than finalizeProject, with the pointer
// plan's use-shape-dependent wordings: `&g` on an extern const struct never
// survives to the whole-program walk, so the located refusal fires at the
// address-taking site itself. FR-70's whole-program addressTakenGlobals gate
// remains the backstop for any address-taking a recovery mode swallows.
// RUN: split-file %s %t
// RUN: not emitrust-import-c --externals-trait %t/addr-ret.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ADDR-RET %s
// RUN: not emitrust-import-c --externals-trait %t/addr-arg.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ADDR-ARG %s
// RUN: not emitrust-import-c --externals-trait %t/addr-cmp.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ADDR-CMP %s
// RUN: not emitrust-import-c --externals-trait %t/array.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ARRAY %s

//--- empty.c
int unrelated(int v) { return v; }

//--- addr-ret.c
// Returning the address of the const global: `E::cfg()` yields a VALUE, and
// no trait item yields the PLACE the returned pointer must reference.
struct S {
  int x;
  int y;
};
extern const struct S cfg;

const struct S *loc(void) { return &cfg; }

int first(void) { return cfg.x; }
// ADDR-RET: addr-ret.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: returned address of a const global

//--- addr-arg.c
// Passing the address into a call: same missing place, different pointer-plan
// wording (the plan classifies by use shape).
struct S {
  int x;
  int y;
};
extern const struct S cfg;
int take(const struct S *p);

int use(void) { return take(&cfg); }

int first(void) { return cfg.x; }
// ADDR-ARG: addr-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of a global variable

//--- addr-cmp.c
// Comparing a pointer against the global's address: identity of environment
// storage is exactly what a by-value getter erases (lwIP's
// `#define IP4_ADDR_ANY (&ip_addr_any)` idiom lands here).
struct S {
  int x;
  int y;
};
extern const struct S cfg;

int same(const struct S *s) { return s == &cfg; }

int first(void) { return cfg.x; }
// ADDR-CMP: addr-cmp.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: comparison of pointers into different objects

//--- array.c
// An ARRAY of const structs keeps the verbatim finalize rejection: element
// access needs a place-yielding projection into environment-owned storage,
// which the by-value getter cannot model, and the frontier stays decided on
// the TYPE, not on which accesses an optimization happened to leave.
struct S {
  int x;
  int y;
};
extern const struct S table[4];

int first(void) { return table[0].x; }
// ARRAY: array.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'table' is referenced but not defined in any translation unit
