// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P11 (00217, type punning): a mismatched-size reinterpreting deref
// `*(T *)p` is accepted when (and only when) the region's base element is
// a byte (i8, a C char array) and the T-sized window fits the region: the
// access widens to sizeof(T) consecutive bytes at the runtime cursor,
// loading via `u32::from_ne_bytes` over the byte run and storing via
// `.to_ne_bytes()` back into it (compound assignments read-modify-write
// through the same window). The IR op granularity for the widened access
// is GREEN's choice — these pins hold the signatures, the byte-region
// shapes, and the staging traffic; the ne_bytes spellings are pinned on
// the emitted Rust in the end-to-end tests. Works over local AND global
// char arrays. Mismatched-size views over non-byte bases stay rejected
// (pointers-reinterpret-invalid.c).

int printf(const char *, ...);

char gbuf[12];

// A u32 load over a local char array at a runtime offset.
unsigned load_local(unsigned long long off) {
  char buf[8];
  int i;
  for (i = 0; i < 8; i++)
    buf[i] = (char)(i + 1);
  return *(unsigned *)(buf + off);
}
// CHECK-LABEL: func.func @load_local
// CHECK-SAME: (%{{[^ :,)]+}}: ui64) -> ui32
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>

// A u32 store through the wide view.
int store_local(unsigned long long off, unsigned v) {
  char buf[8];
  int i;
  for (i = 0; i < 8; i++)
    buf[i] = 0;
  *(unsigned *)(buf + off) = v;
  return buf[5];
}
// CHECK-LABEL: func.func @store_local
// CHECK-SAME: (%{{[^ :,)]+}}: ui64, %{{[^ :,)]+}}: ui32) -> i32
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<8xi8>>

// A compound assignment through the wide view is a read-modify-write
// over the same 4-byte window.
int compound_local(unsigned long long off, unsigned k) {
  char buf[8];
  int i;
  for (i = 0; i < 8; i++)
    buf[i] = (char)(64 + i);
  *(unsigned *)(buf + off) += k;
  return buf[4];
}
// CHECK-LABEL: func.func @compound_local
// CHECK-SAME: (%{{[^ :,)]+}}: ui64, %{{[^ :,)]+}}: ui32) -> i32

// The 00217 shape: a char* local bound into a global char array, the
// wide view applied at pointer-arithmetic offset. Element accesses over
// a global region stage the global's value (the existing staged-copy
// machinery), so the wide load stages gbuf.
unsigned load_global(unsigned long long off) {
  char *data = gbuf;
  return *(unsigned *)(data + off);
}
// CHECK-LABEL: func.func @load_global
// CHECK-SAME: (%{{[^ :,)]+}}: ui64) -> ui32
// CHECK: emitrust.global_load @gbuf : !emitrust.array<12xi8>

// A compound wide assignment over the global region loads the staged
// copy and stores it back (the writeback), so a following direct read
// of gbuf sees the punned bytes.
void compound_global(unsigned long long off, unsigned k) {
  *(unsigned *)(gbuf + off) += k;
}
// CHECK-LABEL: func.func @compound_global
// CHECK: emitrust.global_load @gbuf : !emitrust.array<12xi8>
// CHECK: emitrust.global_store %{{.*}}, @gbuf : !emitrust.array<12xi8>

// Byte coherence: bytes mutated through the wide view are visible to a
// %s print of the same array (which borrows the byte run whole).
void pun_print(unsigned long long off, unsigned k) {
  char t2[10];
  int i;
  for (i = 0; i < 9; i++)
    t2[i] = (char)('0' + i);
  t2[9] = 0;
  *(unsigned *)(t2 + off) += k;
  printf("%s\n", t2);
}
// CHECK-LABEL: func.func @pun_print
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<10xi8>>
// CHECK: emitrust.slice_of %{{.*}} : (!emitrust.lvalue<!emitrust.array<10xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>

int main(void) {
  return 0;
}
// CHECK-LABEL: func.func @c_main
