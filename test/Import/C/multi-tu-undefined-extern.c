// C99-38: "linked into one crate" means every external callee must be present.
// A non-variadic function that is declared and called but defined in no TU is
// a located diagnostic, because the Rust emitter cannot emit a body-less
// function.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-empty.c 2>&1 | FileCheck %s

int undefined_fn(int x);

int main(void) { return undefined_fn(5); }

// CHECK: multi-tu-undefined-extern.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function 'undefined_fn' is referenced but not defined in any translation unit
