// FR-70: an extern GLOBAL that some TU reads and writes as a whole value and
// no TU defines is -- like an FR-52 function -- a fact about the project's
// ENVIRONMENT, not necessarily an error. With --externals-trait the importer
// records it as a requirement: a declaration-only `emitrust.global` marked
// `emitrust.external_requirement`, which emitrust-lower-external-requirements
// later turns into getter/setter items on the Externals trait. This pin
// REVISES FR-52's recorded refusal of globals on the new getter/setter-pair
// shape; the shapes the pair cannot express (address-taken, aggregate) keep
// the verbatim rejection -- see multi-tu-external-requirement-negative.c.
//
// RUN: emitrust-import-c --externals-trait %s \
// RUN:   %S/Inputs/multi-tu-empty.c | FileCheck %s

// The declaration survives as a declaration-only global (no `<init>`),
// appended at module end by finalizeProject, carrying the marker.
// CHECK-DAG: emitrust.global @g_config {emitrust.external_requirement} : i32
// The load and store sites are untouched: the accessor rewrite happens after
// conversion, on the emitted names, exactly like FR-52's call requalification.
// CHECK-DAG: emitrust.global_load @g_config : i32
// CHECK-DAG: emitrust.global_store %{{.*}}, @g_config : i32

extern int g_config;

int bump(int d) {
  int old = g_config;
  g_config = old + d;
  return old;
}

int peek(void) { return g_config; }

// Without the flag the historical whole-program rejection is byte-for-byte
// unchanged, and it is still located at a USE (here the load in peek, the
// first in symbol-use order), not at the declaration.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-empty.c 2>&1 \
// RUN:   | FileCheck --check-prefix=REJECT %s
// REJECT: multi-tu-external-requirement-global.c:30:{{[0-9]+}}: error: unsupported: extern global variable 'g_config' is referenced but not defined in any translation unit
