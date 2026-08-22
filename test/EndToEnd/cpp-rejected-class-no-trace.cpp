// REQUIRES: cargo
// FR-118 byte-diff regression: a HALF-IMPORTED rejected class is a SILENT
// MISCOMPILE across translation units, not merely dead weight in the crate.
//
// Measured on unpatched HEAD with exactly these two sources: the only
// diagnostics are the two `(recovered: item dropped)` warnings for the
// library TU's classes, the crate BUILDS, and it prints an extra
// `lib dtor C 1` / `lib dtor D 2` that the `clang++ -std=c++17` binary does
// not -- `diff` reports the added lines. Nothing is loud about it.
//
// The mechanism this pins: the library TU's `C` is rejected from inside
// `importCXXMethods`, AFTER its `struct_def` (carrying `emitrust.has_drop`)
// is in the module and after `structNameOwnerTuTags["C"]` is claimed. This
// TU's plain POD `struct c` idiomatically renames to `C`, the FR-108
// same-TU collision guard does not apply across TUs, the field shape
// `v:i32;` matches, so the cross-TU dedup MERGES this POD onto the rejected
// class's leftover definition -- and the POD silently inherits an
// `impl Drop`. `D`/`struct d` is the same channel reached through a method
// BODY failure, which is the half no hoisted class-level check could have
// prevented.
//
// Every value derives from argc, so no constant folding can pre-compute the
// answers and hide a miscompile behind a compile-clean crate; `argv` is
// declared (main's standard form) but never touched, since reading it is a
// hard rejection.
//
// The library TU comes FIRST on the command line on purpose: the rejected
// class must claim the emitted name before the POD asks for it, which is the
// order that reproduced the divergence.
//
// RUN: emitrust-cc --recover --emit=crate %S/Inputs/cpp-rejected-class-lib.cpp %s \
// RUN:   -o %t.crate --crate-name cpp_rejected_class --build 2>%t.err
// RUN: FileCheck %s --check-prefix=DIAG --input-file=%t.err
// RUN: clang++ -std=c++17 %S/Inputs/cpp-rejected-class-lib.cpp %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_rejected_class > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct c {
  int v;
};

struct d {
  int w;
};

static int use_c(int n) {
  struct c q;
  q.v = n;
  printf("c %d\n", q.v);
  return q.v;
}

static int use_d(int n) {
  struct d q;
  q.w = n * 2;
  printf("d %d\n", q.w);
  return q.w;
}

int main(int argc, char **argv) {
  int a = use_c(argc);
  int b = use_d(argc);
  printf("sum %d\n", a + b);
  return 0;
}

// DIAG: cpp-rejected-class-lib.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: copy/move/delegating constructor (recovered: item dropped)
// DIAG: cpp-rejected-class-lib.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported pointer expression: CStyleCastExpr (recovered: item dropped)
