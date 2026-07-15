// C99-38: two translation units are merged into one module. An external
// function and global defined in the companion TU satisfy this TU's
// declarations, `main` becomes the single `c_main`, and each TU's file-static
// `helper` survives under a distinct per-TU-mangled name.
// RUN: emitrust-import-c %s %S/Inputs/multi-tu-other.c | FileCheck %s

extern int shared_counter;
int shared_add(int a, int b);
int use_helper(int x);

static int helper(int x) { return x + 1; }

int local_use(int x) { return helper(x); }

int main(void) {
  return shared_add(shared_counter, local_use(use_helper(3)));
}

// The external symbols are unified: one definition each, bare names.
// CHECK-DAG: func.func @shared_add
// CHECK-DAG: func.func @use_helper
// CHECK-DAG: emitrust.global @shared_counter <100 : i32>
// CHECK-DAG: func.func @c_main
// Both file-statics survive distinctly (this TU is tu0, the companion is tu1).
// CHECK-DAG: func.func @tu0_helper
// CHECK-DAG: func.func @tu1_helper
