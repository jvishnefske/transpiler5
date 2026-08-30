// CTS-R2 negative space: anonymous-member flattening admits only what
// single-slot aliasing can model exactly. Since FR-167 a failed flatten
// is no longer fatal on its own — the trial rolls back and the union is
// retried through the ORDINARY union import — so what survives here is
// the intersection: shapes NEITHER model can take. When both fail, the
// TRIAL's diagnostic is re-emitted verbatim, which is why these legs keep
// their historical "union type" wording and location rather than
// `collectUnionSlot`'s arm wordings. An anonymous union whose arms differ
// in mapped type AND width (int against double: no pun, no alias), or
// whose arm is an anonymous struct wider than one field (unnamed to the
// one-slot model), stays rejected; a named union member whose arms fall
// outside the one-slot model (a float arm of a width no integer arm
// shares) keeps that rejection too. A spelling collision between the
// parent and an anonymous member's field is a clang error before import
// (C11 6.7.2.1p13 puts both in one member namespace), also with a
// location. The shapes one model CAN take now import, and are pinned
// positively in union-anon-member-opaque.c.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/mixed-arms.c 2>&1 | FileCheck %s --check-prefix=MIXED
// RUN: not emitrust-import-c %t/wide-arm.c 2>&1 | FileCheck %s --check-prefix=WIDE
// RUN: not emitrust-import-c %t/named-union.c 2>&1 | FileCheck %s --check-prefix=NAMED
// RUN: not emitrust-import-c %t/collision.c 2>&1 | FileCheck %s --check-prefix=COLLIDE

// MIXED: mixed-arms.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union type
// MIXED-NOT: union arm cannot alias the storage slot
// WIDE: wide-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union type
// WIDE-NOT: union with an unnamed arm
// NAMED: named-union.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// COLLIDE: collision.c:{{[0-9]+}}:{{[0-9]+}}: error: member of anonymous struct redeclares 'x'

//--- mixed-arms.c
// Arms of different types cannot alias one slot without punning, and a
// DOUBLE against an int is not a same-width pun either, so the one-slot
// union retry fails as well ("union arm cannot alias the storage slot")
// and the flattening wording is what surfaces. (The same-width int/float
// spelling of this shape now imports — union-anon-member-opaque.c.)
struct s {
  int a;
  union {
    int b;
    double c;
  };
};

struct s g;

//--- wide-arm.c
// A two-field anonymous struct arm overlaps the other arm; that is real
// union storage, not aliasing — and the one-slot union retry cannot take
// an unnamed arm either, so the flattening diagnostic is re-emitted.
struct s {
  union {
    struct {
      int a;
      int b;
    };
    int c;
  };
};

struct s g;

//--- named-union.c
// A named union member whose arms fall outside the one-slot model (a
// float arm against a WIDER integer arm; same-width pairs are admitted
// as puns) stays rejected even after CTS-R3 named-union support.
union u {
  long a;
  float b;
};

struct s {
  union u v;
};

struct s g;

//--- collision.c
// The anonymous member's fields join the parent's member namespace, so
// clang itself rejects the redeclaration before the importer runs.
struct s {
  int x;
  struct {
    int x;
  };
};

struct s g;
