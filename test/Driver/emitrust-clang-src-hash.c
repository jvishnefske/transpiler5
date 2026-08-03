// FR-57: the `src-hash` half of the per-TU cache key covers the WHOLE
// preprocessed input, not just the main file. This test pins the invariant
// that a header-only CONTENT change misses the cache key while every kind of
// workflow noise keeps it: an mtime-only `touch` of a header, a depfile
// request (`-MD -MF <renamed> -MT 'custom target'`), and the depfile's
// absence altogether must all hash identically, because the shim derives the
// dependency list by driving clang's preprocessor internally off the
// classified cc1 line (never by parsing the build's own depfile, whose
// `-MT` target is caller-controlled text) and hashes the CONTENT of every
// dependency as an order-independent digest multiset. A dependency that
// cannot be read at hash time must fail toward "no artifact + warning" —
// never a wrong cache hit — while the real compile's outcome is untouched;
// that path is pinned through the test-only EMITRUST_TEST_UNREADABLE_DEP
// hook (env-gated, like the stats instruments) because no real build can
// delete a header between the delegated compile and the hash in one
// invocation.
//
// A writable header the test edits between compiles:
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: echo '#define DEP_VALUE 1' > %t.dir/dep.h
// RUN: rm -f %t.log
//
// Compile 1: no depfile requested — the internal scan needs no `-MD`.
// RUN: env EMITRUST_REAL_CC=clang EMITRUST_CLANG_LOG=%t.log emitrust-clang -I%t.dir -c %s -o %t.dir/a.o
//
// Compile 2: mtime-only touch of the header keeps the key (content hashing,
// never timestamps).
// RUN: touch %t.dir/dep.h
// RUN: env EMITRUST_REAL_CC=clang EMITRUST_CLANG_LOG=%t.log emitrust-clang -I%t.dir -c %s -o %t.dir/b.o
//
// Compile 3: a depfile under a renamed `-MF` with a multi-word `-MT` target
// is workflow noise for BOTH halves of the key.
// RUN: env EMITRUST_REAL_CC=clang EMITRUST_CLANG_LOG=%t.log emitrust-clang -I%t.dir -MD -MF %t.dir/renamed.d -MT 'custom target' -c %s -o %t.dir/c.o
// RUN: grep "^src-hash: " %t.log | sort -u | count 1
// RUN: grep "^cc1-key: " %t.log | sort -u | count 1
//
// Compile 4: a header CONTENT edit misses the key even though the main
// file's bytes are unchanged.
// RUN: echo '#define DEP_VALUE 2' > %t.dir/dep.h
// RUN: env EMITRUST_REAL_CC=clang EMITRUST_CLANG_LOG=%t.log emitrust-clang -I%t.dir -c %s -o %t.dir/d.o
// RUN: grep "^src-hash: " %t.log | sort -u | count 2
//
// An unreadable dependency at hash time: the delegated compile still
// succeeds and produces its object, but the shim warns and emits NO
// artifact — a missing dependency must never yield a wrong cache hit.
// RUN: env EMITRUST_REAL_CC=clang EMITRUST_CLANG_LOG=%t.log EMITRUST_TEST_UNREADABLE_DEP=dep.h emitrust-clang -I%t.dir -c %s -o %t.dir/e.o 2>%t.err
// RUN: FileCheck %s --check-prefix=MISSING < %t.err
// RUN: ls %t.dir/e.o
// RUN: not ls %t.dir/e.o.emitrust.mlirbc
// MISSING: warning: cannot hash dependency
//
// The artifacts that WERE emitted still carry the import (the header's
// macro reaches the module through the include).
// RUN: emitrust-opt %t.dir/a.o.emitrust.mlirbc -o - | FileCheck %s
// CHECK: emitrust.func @dep_value
// CHECK: emitrust.constant <1 : i32>

#include "dep.h"

int dep_value(void) { return DEP_VALUE; }
