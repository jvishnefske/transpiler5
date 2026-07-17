// RUN: emitrust-import-c %s | FileCheck %s

// CTS-L3: a file-scope `char *s = "..."` is the CTS-P1 read-only literal
// region lifted to module scope: the literal's bytes plus the terminating
// NUL become an immutable `<name>_backing` byte-array global (const — no
// write through the region is ever accepted), and the pointer is the
// usual CTS-P4 stored i64 cursor global, initialized to the initializer's
// byte offset. Dereference and subscript stage the backing like any
// global region base; ++/+= walk the cursor global.

char *s = "hello";
char *t = &"abcd"[2];

// CHECK: emitrust.global const @s_backing <[104 : i8, 101 : i8, 108 : i8, 108 : i8, 111 : i8, 0 : i8]> : !emitrust.array<6xi8>
// CHECK: emitrust.global @s <0 : i64> : i64
// CHECK: emitrust.global const @t_backing <[97 : i8, 98 : i8, 99 : i8, 100 : i8, 0 : i8]> : !emitrust.array<5xi8>
// CHECK: emitrust.global @t <2 : i64> : i64

// A strlen-style walk: `*s` subscripts the staged backing by the cursor
// global's current value, and `s++` is load-add-store on the cursor.
// CHECK-LABEL: func.func @walk
// CHECK: emitrust.global_load @s : i64
// CHECK: emitrust.global_load @s_backing : !emitrust.array<6xi8>
// CHECK: emitrust.subscript
// CHECK: emitrust.global_load @s : i64
// CHECK: arith.addi
// CHECK: emitrust.global_store {{.*}}, @s : i64
int walk(void) {
  int n = 0;
  while (*s) {
    n++;
    s++;
  }
  return n;
}

// Subscripting an offset-initialized cursor: `t[i]` adds the index to the
// stored cursor before staging the backing.
// CHECK-LABEL: func.func @att
// CHECK: emitrust.global_load @t : i64
// CHECK: emitrust.global_load @t_backing : !emitrust.array<5xi8>
// CHECK: emitrust.subscript
int att(int i) { return t[i]; }

int main(void) { return 0; }
