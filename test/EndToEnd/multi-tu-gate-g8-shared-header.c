// W3.1: the intended EndToEnd differential for predicted failure #1
// (shared `T *g;` header pointer global) once G8 AND `deferExternGlobal`'s
// unconditional pointer-extern check are both relaxed (see
// test/Import/C/multi-tu-gate-g8-ptr-global-shared-header.c for the
// located-rejection oracle and the LOUD finding about the second gate).
// Expected-reject for now: no --build, no cargo dependency yet. A later
// wave deletes this reject-pin and turns it into a REQUIRES: cargo
// differential (clang native leg vs emitrust-cc crate build, matching
// test/EndToEnd/multi-tu.c's shape) once the shape below imports cleanly.
// RUN: not emitrust-cc --emit=crate %s %S/Inputs/multi-tu-gate-g8-shared-header-def.c -o %t.crate --crate-name g8_shared 2>&1 | FileCheck %s

extern int *g;

int read_g(void) { return *g; }
int main(void) { return read_g(); }

// CHECK: multi-tu-gate-g8-shared-header.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-typed global variable{{$}}
