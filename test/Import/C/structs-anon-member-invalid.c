// CTS-R2 negative space: anonymous-member flattening admits only what
// single-slot aliasing can model exactly. An anonymous union member
// whose arms differ in type — or whose arm is wider than one flattened
// field — stays rejected with the same located "union type" diagnostic
// union types get everywhere (CTS-R3 territory); a named union member
// whose arms fall outside the one-slot model (a float arm of a width no
// integer arm shares) keeps that rejection too. A spelling collision between the parent and
// an anonymous member's field is a clang error before import (C11
// 6.7.2.1p13 puts both in one member namespace), also with a location.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/mixed-arms.c 2>&1 | FileCheck %s --check-prefix=MIXED
// RUN: not emitrust-import-c %t/wide-arm.c 2>&1 | FileCheck %s --check-prefix=WIDE
// RUN: not emitrust-import-c %t/named-union.c 2>&1 | FileCheck %s --check-prefix=NAMED
// RUN: not emitrust-import-c %t/collision.c 2>&1 | FileCheck %s --check-prefix=COLLIDE

// MIXED: mixed-arms.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union type
// WIDE: wide-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union type
// NAMED: named-union.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union
// COLLIDE: collision.c:{{[0-9]+}}:{{[0-9]+}}: error: member of anonymous struct redeclares 'x'

//--- mixed-arms.c
// Arms of different types cannot alias one slot without punning.
struct s {
  int a;
  union {
    int b;
    float c;
  };
};

struct s g;

//--- wide-arm.c
// A two-field anonymous struct arm overlaps the other arm; that is real
// union storage, not aliasing.
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
