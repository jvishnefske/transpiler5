// RealWorld corpus (Track 4): bitwise CRC-32 over a fixed message.
// Unsigned arithmetic + a subscripted const-char* slice parameter; no dynamic
// memory. Deterministic stdout, doubles as a runtime perf workload.
#include <stdio.h>

static unsigned int crc32(const char *s, int len) {
  unsigned int crc = 0xFFFFFFFFu;
  for (int i = 0; i < len; i++) {
    crc ^= (unsigned char)s[i];
    for (int k = 0; k < 8; k++) {
      unsigned int mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

int main(void) {
  const char *msg = "The quick brown fox";
  printf("%08x\n", crc32(msg, 19));
  return 0;
}
