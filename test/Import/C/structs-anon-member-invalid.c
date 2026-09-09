// CTS-R2 negative space, NARROWED TWICE. Anonymous-member flattening
// admits only what single-slot aliasing can model exactly. Since FR-167
// PHASE 1 a failed flatten is not fatal on its own -- the trial rolls
// back and the union is retried through the ORDINARY union import -- and
// since FR-167 PHASE 2 that retry itself falls back to FR-78's
// sizeof-sized opaque byte blob wherever it used to `return failure()`.
//
// So what survives here is the intersection of THREE models failing, and
// after phase 2 that intersection contains exactly one shape: a spelling
// COLLISION between the parent and an anonymous member's field, which is
// a clang error before the importer ever runs (C11 6.7.2.1p13 puts both
// in one member namespace).
//
// THE OTHER THREE PINS MOVE FORWARD WITH THE FRONTIER rather than
// loosening, and are now exact positive shape checks -- an anonymous
// union whose arms differ in mapped type AND width (int against double),
// an anonymous union whose arm is an anonymous struct wider than one
// field, and a NAMED union member whose arms fall outside the one-slot
// model (a float arm of a width no integer arm shares) all import as
// `["opaque"] [!emitrust.array<8xui8>]` blobs. Note that the anonymous
// spellings land in the parent under phase 1's synthesized `__u<n>`
// name, so both fixes are visible in one line. A regression that
// re-rejected any of them fails here.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/mixed-arms.c | FileCheck %s --check-prefix=MIXED
// RUN: emitrust-import-c %t/wide-arm.c | FileCheck %s --check-prefix=WIDE
// RUN: emitrust-import-c %t/named-union.c | FileCheck %s --check-prefix=NAMED
// RUN: not emitrust-import-c %t/collision.c 2>&1 | FileCheck %s --check-prefix=COLLIDE

// MIXED: emitrust.struct_def @[[MU:Anon[0-9A-F]+]] ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// MIXED: emitrust.struct_def @s ["a", "__u1"] [i32, !emitrust.struct<"[[MU]]">]
// WIDE: emitrust.struct_def @[[WU:Anon[0-9A-F]+]] ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// WIDE: emitrust.struct_def @s ["__u0"] [!emitrust.struct<"[[WU]]">]
// NAMED: emitrust.struct_def @u ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// NAMED: emitrust.struct_def @s ["v"] [!emitrust.struct<"u">]
// COLLIDE: collision.c:{{[0-9]+}}:{{[0-9]+}}: error: member of anonymous struct redeclares 'x'
// COLLIDE-NOT: struct_def

//--- mixed-arms.c
// Arms of different types cannot alias one slot without punning, and a
// DOUBLE against an int is not a same-width pun either, so the one-slot
// union retry fails as well ("union arm cannot alias the storage slot").
// Phase 2 catches it there and the blob takes over. (The same-width
// int/float spelling of this shape already imported one-slot --
// union-anon-member-opaque.c.)
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
// an unnamed arm either. That "union with an unnamed arm" refusal is
// exactly where phase 2's blob now lands: the arm's leaves are reachable
// only through the arm, which the blob access-rejects, so nothing is
// lost by standing in for the storage with bytes.
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
// as puns) reaches `collectUnionSlot` directly -- no flatten, no phase 1
// rollback -- so this leg pins phase 2's fallback on its own.
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
