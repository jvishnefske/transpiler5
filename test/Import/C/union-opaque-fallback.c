// FR-167 PHASE 2: the one-slot union model is now a TRIAL, and a union it
// refuses falls back to FR-78's sizeof-sized opaque byte blob instead of
// killing its whole containing record. Phase 1 put that trial/rollback in
// `collectRecordFields` (the anonymous-union flattening path); this is the
// same idiom one level down, inside `collectUnionSlot`, and it subsumes at
// once every widening candidate the one-slot model had: a POINTER arm,
// arms of DIFFERING SIZES, an aggregate arm that cannot alias a scalar
// slot, and an UNNAMED (anonymous struct/union) arm.
//
// Byte identity is claimed BY CONSTRUCTION -- the fallback runs only where
// the old code returned `failure()` -- and this file pins both halves of
// that claim:
//   * POSITIVE: each of the four families now imports as the SAME
//     `["opaque"] [!emitrust.array<Nxui8>]` struct_def carrying
//     `emitrust.opaque_union`, sized by C's `sizeof`, and the containing
//     record imports with it. FR-215's anonymous-union POINTER arm -- the
//     193-item 38.6x bucket that wore `pointer type outside a parameter
//     position` wording because `collectRecordFields` flattened the union
//     and called `mapType` on the arm -- imports through phase 1's
//     rollback landing in phase 2's fallback.
//   * UNCHANGED: a union the one-slot model ALREADY admits keeps its
//     one-slot field, not a blob (same-width int/float pun, same-type
//     arms, and the CTS-F byte-array mix). The trial handler must not
//     perturb a succeeding path.
//   * NEGATIVE, pinned exactly as hard: a BIT-FIELD arm, an
//     INCOMPLETE-ARRAY arm, an EMPTY union and a C++ union are all
//     INELIGIBLE and never enter the trial at all, so they keep their
//     historical wording AND location. `-NOT` lines guard against blob
//     vocabulary leaking into any of them.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/pointer-arm.c | FileCheck %s --check-prefix=PTR
// RUN: emitrust-import-c %t/differing-sizes.c | FileCheck %s --check-prefix=SIZE
// RUN: emitrust-import-c %t/aggregate-arm.c | FileCheck %s --check-prefix=AGG
// RUN: emitrust-import-c %t/unnamed-arm.c | FileCheck %s --check-prefix=UNNAMED
// RUN: emitrust-import-c %t/anon-pointer-arm.c | FileCheck %s --check-prefix=ANONPTR
// RUN: emitrust-import-c %t/still-one-slot.c | FileCheck %s --check-prefix=ONESLOT
// RUN: not emitrust-import-c %t/bitfield-arm.c 2>&1 | FileCheck %s --check-prefix=BITFIELD
// RUN: not emitrust-import-c %t/incomplete-array-arm.c 2>&1 | FileCheck %s --check-prefix=FAMARM
// RUN: not emitrust-import-c %t/empty.c 2>&1 | FileCheck %s --check-prefix=EMPTY
// RUN: not emitrust-import-c %t/cpp-gate.cpp 2>&1 | FileCheck %s --check-prefix=CPPGATE

//--- pointer-arm.c
// A pointer arm never aliases another slot under the decomposed pointer
// model, so the one-slot model rejected the union AND the record. The blob
// is sizeof(union) = 8; the arms exist only at the type level.
struct S {
  int tag;
  union {
    int i;
    char *p;
  } u;
};

struct S g;

// PTR: emitrust.struct_def @[[U:Anon[0-9A-F]+]] ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// PTR: emitrust.struct_def @S ["tag", "u"] [i32, !emitrust.struct<"[[U]]">]
// PTR: emitrust.global @g : !emitrust.struct<"S">

//--- differing-sizes.c
// Two scalar arms of DIFFERENT widths: neither aliases the other's slot.
struct S {
  int tag;
  union {
    int i;
    long l;
  } u;
};

struct S g;

// SIZE: emitrust.struct_def @[[U:Anon[0-9A-F]+]] ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// SIZE: emitrust.struct_def @S ["tag", "u"] [i32, !emitrust.struct<"[[U]]">]

//--- aggregate-arm.c
// A record arm over a scalar slot: FR-78's `allArmsAggregate` predicate
// missed this because the FIRST arm is a scalar. The blob takes it now.
struct Inner {
  int a;
  int b;
};

struct S {
  int tag;
  union {
    int i;
    struct Inner n;
  } u;
};

struct S g;

// AGG: emitrust.struct_def @Inner ["a", "b"] [i32, i32]
// AGG: emitrust.struct_def @[[U:Anon[0-9A-F]+]] ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// AGG: emitrust.struct_def @S ["tag", "u"] [i32, !emitrust.struct<"[[U]]">]

//--- unnamed-arm.c
// An ANONYMOUS-STRUCT arm of a NAMED union: the one-slot model rejected it
// as "union with an unnamed arm". Its members are reachable only through
// the arm, which the blob access-rejects, so the blob loses nothing.
struct S {
  int tag;
  union {
    struct {
      int a;
      int b;
    };
    long l;
  } u;
};

struct S g;

// UNNAMED: emitrust.struct_def @[[U:Anon[0-9A-F]+]] ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// UNNAMED: emitrust.struct_def @S ["tag", "u"] [i32, !emitrust.struct<"[[U]]">]

//--- anon-pointer-arm.c
// FR-215's 38.6x bucket, verbatim in shape: an ANONYMOUS union member with
// a pointer arm. `collectRecordFields` flattens it and calls `mapType` on
// the arm directly, bypassing `mapStructFieldType`'s data-pointer -> i64
// shortcut, so the flatten fails with POINTER wording. Phase 1's rollback
// re-routes to `collectUnionSlot`, which used to reject it a second time
// ("union with a pointer arm") -- exactly where phase 2's blob now lands.
struct S {
  int tag;
  union {
    int i;
    char *p;
  };
};

struct S g;

// ANONPTR: emitrust.struct_def @[[U:Anon[0-9A-F]+]] ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// ANONPTR: emitrust.struct_def @S ["tag", "__u1"] [i32, !emitrust.struct<"[[U]]">]

//--- still-one-slot.c
// The trial must not perturb what already succeeded. All three of these
// are one-slot admissions and must stay one-slot fields, NOT blobs: a
// same-width int/float pun, identical-typed arms, and the CTS-F byte-array
// mix (a constant integer array arm whose total width equals the integer
// slot's).
union Pun {
  int b;
  float c;
};

union Same {
  int x;
  int y;
};

union Bytes {
  unsigned char raw[4];
  unsigned w;
};

union Pun p;
union Same s;
union Bytes b;

// ONESLOT: emitrust.struct_def @Pun ["b"] [i32]
// ONESLOT: emitrust.struct_def @Same ["x"] [i32]
// ONESLOT: emitrust.struct_def @Bytes ["w"] [ui32]
// ONESLOT-NOT: emitrust.opaque_union

//--- bitfield-arm.c
// INELIGIBLE: a bit-field arm is not addressable storage a byte blob can
// stand in for, and the rejection's ARM location must survive. The union
// never enters the trial, so the wording is the untouched original.
union B {
  unsigned bf : 3;
  int c;
};

union B g;

// BITFIELD: bitfield-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union with a bit-field arm
// BITFIELD-NOT: struct_def

//--- incomplete-array-arm.c
// INELIGIBLE: `sizeof` does not cover a flexible tail, so a blob sized by
// it would silently drop storage the C program can reach. Keeps the
// `mapType` wording at the ARM.
struct S {
  int tag;
  union {
    int i;
    char tail[];
  } u;
};

struct S g;

// FAMARM: incomplete-array-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-constant array size
// FAMARM-NOT: struct_def

//--- empty.c
// INELIGIBLE: an empty union (the GNU extension clang accepts in C) has no
// arms to stand in for and no bytes to alias.
union E {
};

union E g;

// EMPTY: empty.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union with no members
// EMPTY-NOT: struct_def

//--- cpp-gate.cpp
// INELIGIBLE: C++ is out of scope, exactly as FR-78 and phase 1 scoped it
// out -- the blob sits next to the unmeasured destructor/copy-ctor member
// surface (`kHasDropAttrName`, `Copy` derivation).
struct S {
  int tag;
  union {
    int i;
    long l;
  } u;
};

S g;

// CPPGATE: cpp-gate.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union arms of differing sizes
// CPPGATE-NOT: struct_def
