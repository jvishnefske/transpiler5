// FR-134: the NEGATIVE leg of emitrust-canonical-roundtrip -- the one that
// proves the check is not vacuous.
//
// The pass builds its fresh context with unregistered dialects FORBIDDEN. That
// is the whole reason it can detect anything: the tempting alternative,
// `allowUnregisteredDialects(true)`, would let an op the context does not know
// survive the round-trip as opaque generic text and compare EQUAL, so every
// unknown construct would silently pass. Refusing instead means a dialect
// outside the project's enumerated universe fails loudly and names itself.
//
// This also documents the maintenance obligation: if a future stage introduces
// a new dialect, this pass's `registerProjectDialects` must learn it, and the
// symptom will be exactly the diagnostic below rather than a silent gap.
//
// RUN: not emitrust-opt --allow-unregistered-dialect \
// RUN:     --emitrust-canonical-roundtrip %s 2>&1 | FileCheck %s

// CHECK: error: operation being parsed with an unregistered dialect
// CHECK: error: canonical round-trip failed: the module's own generic form does not parse back
func.func @has_unregistered(%a: i32) -> i32 {
  %r = "totally.unknown"(%a) : (i32) -> i32
  return %r : i32
}
