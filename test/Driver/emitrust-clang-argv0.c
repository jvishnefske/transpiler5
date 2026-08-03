// FR-56 argv0 aliasing: `make CC=cc` with `cc` symlinked to emitrust-clang
// must behave exactly like invoking emitrust-clang directly. The shim
// classifies its own program name with clang's OWN mechanism
// (`ToolChain::getTargetAndModeFromProgramName`, the ends-with suffix table
// clang's main uses — never a hand-rolled name table beyond the symlink
// dispatch): the parsed name feeds the classification driver via
// `setTargetAndMode`, and its `--driver-mode=`/target-prefix implications
// are re-inserted into the DELEGATED argv, because the subprocess runs
// under the real clang's own argv[0] and would otherwise lose them. This
// test pins: a `cc`- and a `gcc`-named symlink produce artifact AND object
// bytes identical to the direct invocation; a `g++`-named symlink drives
// g++ mode through BOTH sides — the delegated compile builds the .c as
// C++, and the classification sees `-x c++` and correctly emits no
// artifact (the shim imports C translation units only).
//
// RUN: rm -rf %t.bin && mkdir -p %t.bin
// RUN: ln -sf emitrust-clang %t.bin/cc
// RUN: ln -sf emitrust-clang %t.bin/gcc
// RUN: ln -sf emitrust-clang %t.bin/g++
//
// The direct invocation is the reference:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -DANSWER=42 -c %s -o %t.direct.o
//
// `cc` builds identically — object and artifact byte-for-byte:
// RUN: env EMITRUST_REAL_CC=clang %t.bin/cc -DANSWER=42 -c %s -o %t.cc.o
// RUN: cmp %t.cc.o %t.direct.o
// RUN: cmp %t.cc.o.emitrust.mlirbc %t.direct.o.emitrust.mlirbc
//
// ... and so does `gcc`:
// RUN: env EMITRUST_REAL_CC=clang %t.bin/gcc -DANSWER=42 -c %s -o %t.gcc.o
// RUN: cmp %t.gcc.o %t.direct.o
// RUN: cmp %t.gcc.o.emitrust.mlirbc %t.direct.o.emitrust.mlirbc
//
// `g++` selects g++ mode via clang's suffix table: the delegated compile
// treats the .c input as C++ (so the object DIFFERS from the C compile)
// and the classification sees a C++ job and emits no artifact:
// RUN: rm -f %t.gxx.o.emitrust.mlirbc
// RUN: env EMITRUST_REAL_CC=clang %t.bin/g++ -DANSWER=42 -c %s -o %t.gxx.o
// RUN: not cmp %t.gxx.o %t.direct.o
// RUN: not ls %t.gxx.o.emitrust.mlirbc
//
// The artifact still carries the import (sanity on the aliased path):
// RUN: emitrust-opt %t.cc.o.emitrust.mlirbc -o - | FileCheck %s
// CHECK: emitrust.func @argv0_answer
// CHECK: emitrust.constant <42 : i32>

int argv0_answer(void) { return ANSWER; }
