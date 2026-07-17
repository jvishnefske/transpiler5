// CTS-F2: an `extern` object declaration that nothing references demands no
// definition in any translation unit — the referenced-only policy
// system-header declarations already follow (C99-39). Nothing is emitted for
// it, in single-file and project imports alike.
// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-empty.c | FileCheck %s

extern int x;

int main(void) { return 0; }

// CHECK-NOT: emitrust.global
// CHECK-LABEL: func.func @c_main() -> i32
// CHECK: return
// CHECK-NOT: emitrust.global
