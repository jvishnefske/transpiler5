// FR-167: an anonymous union MEMBER whose arms cannot flatten into the
// parent is no longer a RECORD-level rejection. The defect was ROUTING,
// not a predicate: FR-78's opaque diversion lives inside
// `collectUnionSlot`, which is reached only for a union that has a TYPE
// — a NAMED member — so the anonymous-member flattening path in
// `collectRecordFields` never saw it. Two spellings one word apart
// ("} u;" vs "};") disagreed: the named one imported as an opaque blob,
// the anonymous one killed the whole record. Now the flatten runs as a
// TRIAL under a silencing handler; on failure its state rolls back and
// the union is appended as a synthesized-named member (`__u<n>`) through
// the ORDINARY union import. This file pins:
//   * the two spellings produce the SAME union struct_def (the same
//     shape-keyed `Anon<hash>`, the same opaque blob) — that equality is
//     the whole point of the fix;
//   * a shape the flatten cannot take but the one-slot union CAN (a
//     same-width int/float pun) now imports instead of rejecting;
//   * every residual rejection keeps its EXACT original wording and
//     location, because the trial's diagnostics are re-emitted verbatim
//     when the union import also fails — an unnamed (anonymous-struct)
//     arm keeps the flattening `union type` wording, NOT
//     `collectUnionSlot`'s "union with an unnamed arm"; a bit-field arm
//     keeps its ARM location; a pointer arm keeps the flattening
//     `pointer type outside a parameter position` wording, NOT
//     "union with a pointer arm";
//   * C++ is OUT of scope (FR-78 already scopes out C++, and a
//     non-flattening anonymous union newly reaching `collectUnionSlot`
//     would meet the unmeasured destructor/copy-ctor member surface), so
//     the C++ spelling keeps the record-level rejection.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/anon.c | FileCheck %s --check-prefix=ANON
// RUN: emitrust-import-c %t/named.c | FileCheck %s --check-prefix=NAMED
// RUN: emitrust-import-c %t/pun-arms.c | FileCheck %s --check-prefix=PUN
// RUN: not emitrust-import-c %t/unnamed-arm.c 2>&1 | FileCheck %s --check-prefix=UNNAMEDARM
// RUN: not emitrust-import-c %t/bitfield-arm.c 2>&1 | FileCheck %s --check-prefix=BITFIELD
// RUN: not emitrust-import-c %t/pointer-arm.c 2>&1 | FileCheck %s --check-prefix=POINTER
// RUN: not emitrust-import-c %t/cpp-gate.cpp 2>&1 | FileCheck %s --check-prefix=CPPGATE

//--- anon.c
// The anonymous spelling. The union is imported as its own opaque
// struct_def and appended to the parent under a synthesized name; the
// arm spellings never reach the IR.
struct Inner {
  int a;
  int b;
};

struct T {
  int hdr;
  union {
    struct Inner s;
    unsigned char raw[8];
  };
};

struct T g;

// ANON: emitrust.struct_def @Inner ["a", "b"] [i32, i32]
// ANON: emitrust.struct_def @[[U:Anon[0-9A-F]+]] ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// ANON: emitrust.struct_def @T ["hdr", "__u1"] [i32, !emitrust.struct<"[[U]]">]
// ANON: emitrust.global @g : !emitrust.struct<"T">

//--- named.c
// The NAMED spelling of the very same union: byte-identical arms, one
// word of difference. The union struct_def must carry the SAME
// shape-keyed name and the same blob as anon.c's — only the parent's
// field spelling differs (`u` instead of the synthesized `__u1`).
struct Inner {
  int a;
  int b;
};

struct T {
  int hdr;
  union {
    struct Inner s;
    unsigned char raw[8];
  } u;
};

struct T g;

// NAMED: emitrust.struct_def @Inner ["a", "b"] [i32, i32]
// NAMED: emitrust.struct_def @[[U:Anon[0-9A-F]+]] ["opaque"] [!emitrust.array<8xui8>] {{.*}}emitrust.opaque_union}
// NAMED: emitrust.struct_def @T ["hdr", "u"] [i32, !emitrust.struct<"[[U]]">]
// NAMED: emitrust.global @g : !emitrust.struct<"T">

//--- pun-arms.c
// Arms of DIFFERENT mapped types cannot alias one flattened slot, so the
// trial fails — but the ordinary one-slot union import admits a
// same-width int/float PUN. The record imports; the union keeps its
// one-slot (non-opaque) model.
struct s {
  int a;
  union {
    int b;
    float c;
  };
};

struct s g;

// PUN: emitrust.struct_def @[[U:Anon[0-9A-F]+]] ["b"] [i32]
// PUN: emitrust.struct_def @s ["a", "__u1"] [i32, !emitrust.struct<"[[U]]">]
// PUN: emitrust.global @g : !emitrust.struct<"s">

//--- unnamed-arm.c
// An anonymous-STRUCT arm: the flatten rejects it as wider than one slot,
// and `collectUnionSlot` rejects it as an unnamed arm. Because the union
// import also fails, the TRIAL's diagnostic is re-emitted verbatim, so
// this shape keeps the historical CTS-R2 wording and location.
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

// UNNAMEDARM: unnamed-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union type
// UNNAMEDARM-NOT: union with an unnamed arm

//--- bitfield-arm.c
// A bit-field arm is not addressable storage for either model. The trial
// reports it at the ARM's own location; that location survives.
struct s {
  int hdr;
  union {
    unsigned bf : 3;
    int c;
  };
};

struct s g;

// BITFIELD: bitfield-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union with a bit-field arm

//--- pointer-arm.c
// A pointer arm: the trial's `mapType` rejects the pointer leaf at the
// ARM declaration, while `collectUnionSlot` would say "union with a
// pointer arm" at the union. The trial's wording and location win.
struct A {
  int x;
  int y;
};

struct s {
  int hdr;
  union {
    struct A a;
    char *p;
  };
};

struct s g;

// POINTER: pointer-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position
// POINTER-NOT: union with a pointer arm

//--- cpp-gate.cpp
// C++ is out of scope: the anonymous union keeps the record-level
// rejection so nothing newly reaches `collectUnionSlot` next to the
// destructor/copy-ctor member surface.
struct Inner {
  int a;
  int b;
};

struct T {
  int hdr;
  union {
    struct Inner s;
    unsigned char raw[8];
  };
};

T g;

// CPPGATE: cpp-gate.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: union type
