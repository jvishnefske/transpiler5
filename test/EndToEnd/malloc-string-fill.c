// REQUIRES: cargo
// FR-64 differential end-to-end test (the W4.5 heap memory-model change): a
// runtime-sized `char` buffer filled with a constant ASCII byte and consumed
// as a C string lifts WHOLE to an idiomatic `let a: String = "a".repeat(len as
// usize)` — the malloc, the counted fill loop, and the NUL terminator fused,
// `free` a no-op drop, `puts` a `println!`. The fill count derives from `argc`
// so it cannot fold at import time. The crate's stdout must byte-match the
// natively compiled C program for len > 0 AND len == 0 (the empty string).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/malloc_string_fill > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdlib.h>

int puts(const char *);

// The intended constant-fill idiom: malloc(len+1), a canonical [0,len) counted
// fill loop writing a compile-time-constant ASCII byte, a NUL terminator, then
// a string consumer and a free.
void alloc_string_a(unsigned int len) {
  char *a = malloc(len + 1);
  for (int i = 0; i < len; ++i)
    a[i] = 'a';
  a[len] = '\0';
  puts(a);
  free(a);
}

int main(int argc, char **argv) {
  alloc_string_a((unsigned int)argc);       // len = argc (>= 1), non-foldable
  alloc_string_a((unsigned int)(argc * 4)); // a wider multi-byte fill
  alloc_string_a(0u);                        // len == 0: the empty string
  return 0;
}
