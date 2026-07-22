// RealWorld corpus (Track 4): base64-encode a fixed buffer into stdout.
// Fixed arrays + putchar; no dynamic memory. Deterministic, a perf workload.
#include <stdio.h>

int main(void) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned char in[] = "Hello, World";
  int n = 12;
  for (int i = 0; i < n; i += 3) {
    int b0 = in[i];
    int b1 = (i + 1 < n) ? in[i + 1] : 0;
    int b2 = (i + 2 < n) ? in[i + 2] : 0;
    putchar(tbl[b0 >> 2]);
    putchar(tbl[((b0 & 3) << 4) | (b1 >> 4)]);
    putchar((i + 1 < n) ? tbl[((b1 & 15) << 2) | (b2 >> 6)] : '=');
    putchar((i + 2 < n) ? tbl[b2 & 63] : '=');
  }
  putchar('\n');
  return 0;
}
