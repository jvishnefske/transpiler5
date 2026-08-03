// FR-56 import-crash isolation: an importer crash on one translation unit
// must cost that TU's artifact and NOTHING else -- never the build. The
// shim forks the whole artifact side-emission (dependency scan + import +
// pipeline + serialization + embedding) per compile job, so a crash
// anywhere in it is observed by the parent as an abnormal child exit: the
// parent warns, removes any partial sidecar, and returns the delegated
// real clang's exit code unchanged. The crash is forced through the
// test-only EMITRUST_TEST_CRASH_IMPORT env hook (an abort() in the child
// before the scan, precedented by the other env-gated instruments) because
// no cheap C input crashes the importer on demand.
//
// A forced crash: the build still succeeds, the object is byte-identical
// to a plain-clang compile (no .emitrust section was ever added), the
// warning names the input, and no artifact -- not even a truncated one --
// is left behind.
// RUN: env EMITRUST_REAL_CC=clang EMITRUST_TEST_CRASH_IMPORT=1 emitrust-clang -c %s -o %t.o 2>%t.err
// RUN: FileCheck %s --check-prefix=CRASH < %t.err
// CRASH: emitrust-clang: warning: emitrust artifact emission crashed for '{{.*}}emitrust-clang-crash-isolation.c'; no artifact written
// RUN: not ls %t.o.emitrust.mlirbc
// RUN: clang -c %s -o %t.ref.o
// RUN: cmp %t.o %t.ref.o
//
// Without the hook the fork path IS the normal path: the artifact appears
// and carries the import.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.ok.o
// RUN: emitrust-opt %t.ok.o.emitrust.mlirbc -o - | FileCheck %s
// CHECK: emitrust.func @crash_fn
// CHECK: emitrust.constant <7 : i32>

int crash_fn(void) { return 7; }
