// FR-79: a CONST struct-typed extern global that no TU defines is -- like
// FR-70's scalars -- a requirement on the ENVIRONMENT, expressible as a
// GETTER-ONLY trait item: imported structs are Copy, so a by-value return is
// a faithful read, and const-ness means no writer ever needs the setter a
// multi-field by-value write could tear. This pin REVISES the aggregate arm
// of FR-70's recorded refusal for exactly the const case; FR-81 later
// flipped the NON-const struct extern too (whole-value getter/setter pair,
// multi-tu-external-requirement-nonconst-struct.c), while address-taken and
// array-of-struct shapes keep rejecting (multi-tu-external-requirement-
// const-struct-negative.c). The pin also holds the FIELD-PROJECTION shape: `g.field`
// imports as a whole-value load into a temporary, then member on the
// temporary -- so every surviving symbol use is a load the FR-70 rewrite
// already covers.
//
// RUN: emitrust-import-c --externals-trait %s \
// RUN:   %S/Inputs/multi-tu-empty.c | FileCheck %s

// The declaration survives as a declaration-only global (no `<init>`)
// carrying BOTH the requirement marker and the `const` marker -- the const
// fact is what licenses the getter-only contract, and
// emitrust-lower-external-requirements enforces it (a store on a const
// requirement is refused, never silently given a setter).
// CHECK-DAG: emitrust.global const @ip_addr_any {emitrust.external_requirement} : !emitrust.struct<"ip_addr">
// Whole-struct read: a plain whole-value load.
// CHECK-DAG: emitrust.global_load @ip_addr_any : !emitrust.struct<"ip_addr">
// Field projection: the SAME whole-value load bound to a temporary, then
// member on the temporary -- never a projection into the global place.
// CHECK-DAG: emitrust.member %{{.*}}["addr"] : (!emitrust.lvalue<!emitrust.struct<"ip_addr">>) -> !emitrust.lvalue<ui32>

struct ip_addr {
  unsigned int addr;
  int kind;
};
extern const struct ip_addr ip_addr_any;

int is_any(unsigned int a) { return a == ip_addr_any.addr; }

struct ip_addr copy_any(void) { return ip_addr_any; }

// Without the flag the historical whole-program rejection is byte-for-byte
// unchanged, and it is still located at a USE (the first in symbol-use
// order), not at the declaration.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-empty.c 2>&1 \
// RUN:   | FileCheck --check-prefix=REJECT %s
// REJECT: multi-tu-external-requirement-const-struct.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'ip_addr_any' is referenced but not defined in any translation unit
