// FR-81: a NON-const struct-typed extern global that no TU defines is a
// requirement on the ENVIRONMENT, expressible as a whole-value
// GETTER/SETTER pair: imported structs are Copy, and for the
// single-threaded programs the importer accepts, sequential whole-value
// get/modify/set is EXACT. This pin REVISES the aggregate arm of FR-70's
// recorded refusal for the non-const case (FR-79 already flipped const),
// closing the requirement-type matrix: non-const scalar (FR-70), const
// struct (FR-79/80), non-const struct (this pin). Address-taken and
// array-of-struct shapes keep the verbatim rejection
// (multi-tu-external-requirement-nonconst-struct-negative.c).
//
// The pin holds the SEQUENCING contract that makes the pair exact: a field
// write is a staged read-modify-write -- whole-value load bound to a
// temporary, member-assign on the temporary, ONE whole-value store back --
// and in a multi-access statement every RHS read is a FRESH whole-value
// load, so the C-sequenced result is produced by construction, never by an
// aliasing argument.
//
// RUN: emitrust-import-c --externals-trait %s \
// RUN:   %S/Inputs/multi-tu-empty.c | FileCheck %s

struct ip_globals {
  int ttl;
  unsigned int addr;
};
extern struct ip_globals ip_data;

// Field read: the whole-value load bound to a temporary, then member on the
// temporary -- never a projection into the global place.
// CHECK-LABEL: func.func @get_ttl
// CHECK:         %[[R:.*]] = emitrust.global_load @ip_data : !emitrust.struct<"ip_globals">
// CHECK:         emitrust.assign %[[T:.*]] = %[[R]] : !emitrust.lvalue<!emitrust.struct<"ip_globals">>
// CHECK:         emitrust.member %[[T]]["ttl"]
// CHECK-NOT:     emitrust.global_store
int get_ttl(void) { return ip_data.ttl; }

// Field write: staged read-modify-write. The temporary is seeded from a
// whole-value load, the member-assign lands on the temporary, and exactly
// ONE whole-value store writes it back.
// CHECK-LABEL: func.func @set_ttl
// CHECK:         %[[C:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"ip_globals">>
// CHECK:         %[[L:.*]] = emitrust.global_load @ip_data : !emitrust.struct<"ip_globals">
// CHECK:         emitrust.assign %[[C]] = %[[L]]
// CHECK:         %[[M:.*]] = emitrust.member %[[C]]["ttl"]
// CHECK:         emitrust.assign %[[M]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK:         %[[W:.*]] = emitrust.load %[[C]] : (!emitrust.lvalue<!emitrust.struct<"ip_globals">>) -> !emitrust.struct<"ip_globals">
// CHECK:         emitrust.global_store %[[W]], @ip_data : !emitrust.struct<"ip_globals">
void set_ttl(int t) { ip_data.ttl = t; }

// Compound assign through a field lowers via the SAME read-modify-write:
// one staged copy serves both the read and the write of `+=`.
// CHECK-LABEL: func.func @bump
// CHECK:         %[[C2:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"ip_globals">>
// CHECK:         emitrust.global_load @ip_data
// CHECK:         %[[M2:.*]] = emitrust.member %[[C2]]["ttl"]
// CHECK:         emitrust.load %[[M2]]
// CHECK:         emitrust.assign %[[M2]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK:         emitrust.global_store %{{.*}}, @ip_data
void bump(void) { ip_data.ttl += 2; }

// Multi-access statement (a pending set while the RHS reads twice): the LHS
// copy is staged FIRST, then each RHS read gets its own FRESH whole-value
// load -- three loads, one store, C-sequenced by construction.
// CHECK-LABEL: func.func @swapish
// CHECK:         %[[LHS:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"ip_globals">>
// CHECK:         emitrust.global_load @ip_data
// CHECK:         emitrust.member %[[LHS]]["ttl"]
// CHECK:         emitrust.global_load @ip_data
// CHECK:         emitrust.member %{{.*}}["addr"]
// CHECK:         emitrust.global_load @ip_data
// CHECK:         emitrust.member %{{.*}}["ttl"]
// CHECK:         %[[W2:.*]] = emitrust.load %[[LHS]]
// CHECK:         emitrust.global_store %[[W2]], @ip_data
void swapish(void) { ip_data.ttl = (int)ip_data.addr + ip_data.ttl; }

// Whole-struct read: a plain whole-value load.
// CHECK-LABEL: func.func @snapshot
// CHECK:         emitrust.global_load @ip_data : !emitrust.struct<"ip_globals">
struct ip_globals snapshot(void) { return ip_data; }

// Whole-struct write: a plain whole-value store.
// CHECK-LABEL: func.func @restore
// CHECK:         emitrust.global_store %{{.*}}, @ip_data : !emitrust.struct<"ip_globals">
void restore(struct ip_globals s) { ip_data = s; }

// The declaration survives as a declaration-only global (no `<init>`),
// appended at module end by finalizeProject, carrying the requirement
// marker and -- unlike FR-79's const structs -- NO `const` marker: stores
// must verify, and the lowering derives the setter item from them.
// CHECK: emitrust.global @ip_data {emitrust.external_requirement} : !emitrust.struct<"ip_globals">

// Without the flag the historical whole-program rejection is byte-for-byte
// unchanged, and it is still located at a USE (the first in symbol-use
// order), not at the declaration.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-empty.c 2>&1 \
// RUN:   | FileCheck --check-prefix=REJECT %s
// REJECT: multi-tu-external-requirement-nonconst-struct.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'ip_data' is referenced but not defined in any translation unit
