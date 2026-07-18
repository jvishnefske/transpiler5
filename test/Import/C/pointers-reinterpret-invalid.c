// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/past-end.c 2>&1 | FileCheck %s --check-prefix=PASTEND
// RUN: not emitrust-import-c %t/wide-nonbyte.c 2>&1 | FileCheck %s --check-prefix=WIDE

// Boundaries of the byte-pun widening (CTS-P11). Mismatched-size
// reinterpreting derefs are allowed ONLY over byte (i8) regions, and only
// when the widened window provably fits: a 4-byte view at a
// compile-time-known offset that overruns the array is a located
// rejection, and a wider-than-element view over a non-byte base stays in
// the existing reinterpret rejection family.

// A 4-byte access at offset 5 of a char[8] reaches byte 8: one past the
// end.
// PASTEND: past-end.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 4-byte access at offset 5 runs past the end of 'buf' (8 bytes)

//--- past-end.c
int main(void) {
  char buf[8];
  buf[0] = 1;
  *(unsigned *)(buf + 5) = 7u;
  return buf[0];
}

// A wide view over a non-byte base (long long over int storage) is not a
// byte pun; it keeps the reinterpret rejection.
// WIDE: wide-nonbyte.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer cast reinterprets the pointee ('long long' over 'int' storage)

//--- wide-nonbyte.c
int main(void) {
  int arr[4];
  int *ip = arr;
  arr[0] = 1;
  *(long long *)ip = 5;
  return arr[0];
}
