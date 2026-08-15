// FR-75: a declaration-only `void *` parameter under a trait policy joins
// the byte-slice convention by CALL-SITE CONSENSUS. FR-71's body scan
// cannot run without a body, so the admission is a TU-level scan of every
// call site instead: the parameter maps to `&mut [u8]` (`&[u8]` for
// `const void *` — constness from the void pointee qualifier, exactly
// like the FR-71 branch) iff EVERY call site in the TU passes an
// admissible byte view (byte array or byte pointer) with ONE consistent
// element. Any mixed or non-byte call site keeps the verbatim
// void-pointer rejection (see the MIXED case), and without the trait flag
// nothing changes at all (NOFLAG) — which is what keeps the bin-mode
// DECLONLY pin in void-param-invalid.c and the Driver ledger byte-stable.
//
// RUN: split-file %s %t
// RUN: emitrust-import-c --externals-trait %t/lib.c %t/other.c \
// RUN:   | FileCheck %s
//
// CHECK-DAG: func.func private @vset(!emitrust.mut_ref<!emitrust.slice<ui8>>, ui8, ui32) attributes {emitrust.external_requirement}
// CHECK-DAG: func.func private @vsum(!emitrust.ref<!emitrust.slice<ui8>>, ui32) -> i32 attributes {emitrust.external_requirement}
//
// Call sites lower as region views at the argument's cursor, exactly like
// a spelled-out byte-pointer parameter's.
// CHECK-LABEL: func.func @use_it
// CHECK: emitrust.slice_of mut %{{.*}}[%c0_i64{{[_0-9]*}}] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK-NEXT: call @vset(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui8, ui32) -> ()
// CHECK: %[[CUR:.*]] = arith.addi %{{.*}}, %{{.*}} : i64
// CHECK-NEXT: emitrust.slice_of mut %{{.*}}[%[[CUR]]] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<ui8>>
// CHECK: call @vset(%{{.*}}, %{{.*}}, %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui8, ui32) -> ()
// CHECK: emitrust.slice_of %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<8xui8>>, i64) -> !emitrust.ref<!emitrust.slice<ui8>>
// CHECK: call @vsum(%{{.*}}, %{{.*}}) : (!emitrust.ref<!emitrust.slice<ui8>>, ui32) -> i32
//
// FRONTIER: one int-array call site breaks the consensus for the whole
// parameter — the verbatim rejection, located at the parameter. (Two
// files again: only the project entry point applies the trait policy, so
// this run genuinely reaches — and declines — the consensus scan.)
// RUN: not emitrust-import-c --externals-trait %t/mixed.c %t/other.c 2>&1 \
// RUN:   | FileCheck --check-prefix=MIXED %s
// MIXED: mixed.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: void pointer parameter
//
// Without the trait flag the consensus never runs; the historical
// rejection is unchanged even though every call site passes a byte view.
// RUN: not emitrust-import-c %t/lib.c %t/other.c 2>&1 \
// RUN:   | FileCheck --check-prefix=NOFLAG %s
// NOFLAG: lib.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: void pointer parameter

//--- lib.c
void vset(void *dst, unsigned char c, unsigned int n);
int vsum(const void *src, unsigned int n);

int use_it(void) {
  unsigned char b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  vset(b, 0, 8u);
  vset(b + 2, 1, 4u);
  return vsum(b, 8u);
}

//--- other.c
// Defines nothing lib.c needs, so both prototypes stay undefined
// project-wide and become requirements.
int er_void_dummy(int v) { return v + 1; }

//--- mixed.c
void vset(void *dst, unsigned char c, unsigned int n);

int use_it(void) {
  unsigned char b[4] = {0, 0, 0, 0};
  int w[4] = {0, 0, 0, 0};
  vset(b, 0, 4u);
  vset(w, 0, 16u);
  return w[0];
}
