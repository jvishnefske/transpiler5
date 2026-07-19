// RUN: split-file %s %t
// RUN: emitrust-import-c %t/main.c %t/lib.c | FileCheck %s

// C99-18 x C99-38: inline across translation units. A `static inline`
// helper defined (differently) in two TUs has internal linkage in each,
// so the merged module keeps both bodies under distinct per-TU-mangled
// names — exactly like any other file-static. A plain `inline`
// definition confined to one TU imports once under its bare external
// name, and cross-TU calls reach it like an ordinary definition.

//--- main.c
int use_lib(int x);

static inline int helper(int x) {
  return x + 1;
}

inline int shared_inline(int x) {
  return x * 2;
}

int main(void) {
  return helper(1) + use_lib(2) + shared_inline(3);
}

//--- lib.c
int shared_inline(int x);

static inline int helper(int x) {
  return x + 100;
}

int use_lib(int x) {
  return shared_inline(helper(x));
}

// Both static inline helpers survive under per-TU mangled names with
// their own bodies; the plain inline definition is the single external
// one.
// CHECK-DAG: func.func @tu0_helper
// CHECK-DAG: func.func @tu1_helper
// CHECK-DAG: func.func @shared_inline
// CHECK-DAG: func.func @c_main
// CHECK-DAG: func.func @use_lib
