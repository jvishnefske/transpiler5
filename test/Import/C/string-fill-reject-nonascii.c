// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-64 rejection (non-ASCII fill / soundness gate): a fill byte outside ASCII
// 0x01..0x7F (`(char)200`) is not a valid single-byte UTF-8 scalar, so a
// `String` built from it would NOT have the same bytes as the C buffer. The
// buffer is NOT lifted and keeps the historical located rejection — the safe
// direction is to reject, never to silently mistranslate the bytes.
#include <stdlib.h>
int puts(const char *);

void f(unsigned int len) {
  char *a = malloc(len + 1);
  for (int i = 0; i < len; ++i)
    a[i] = (char)200; // non-ASCII: String bytes would differ from C bytes
  a[len] = '\0';
  puts(a);
  free(a);
}
// CHECK: error: unsupported: allocation size is not a compile-time constant
