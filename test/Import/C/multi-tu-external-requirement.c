// FR-52: a non-variadic external FUNCTION that some TU calls and none defines
// is a fact about the project's ENVIRONMENT, not necessarily an error. With
// --externals-trait the importer records it as a requirement -- a body-less
// func.func marked `emitrust.external_requirement`, which the
// emitrust-lower-external-requirements pass later turns into a Rust trait --
// instead of failing the whole import.
//
// RUN: emitrust-import-c --externals-trait %s \
// RUN:   %S/Inputs/multi-tu-external-requirement-other.c | FileCheck %s

// The declaration survives, body-less, carrying the marker.
// CHECK-DAG: func.func private @host_scale(i32) -> i32 attributes {emitrust.external_requirement}
// The call site is untouched: requalification happens after conversion, on
// the emitted names.
// CHECK-DAG: call @host_scale(

int host_scale(int v);

int scaled(int v) { return host_scale(v) + 1; }

// Without the flag the historical whole-program rejection is unchanged, and
// it is still located at the first USE, not at the declaration.
// RUN: not emitrust-import-c %s \
// RUN:   %S/Inputs/multi-tu-external-requirement-other.c 2>&1 \
// RUN:   | FileCheck --check-prefix=REJECT %s
// REJECT: multi-tu-external-requirement.c:19:{{[0-9]+}}: error: unsupported: function 'host_scale' is referenced but not defined in any translation unit
