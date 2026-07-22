// RealWorld corpus (Track 4): word count over a fixed string.
// Char scanning over a const-char* slice; no dynamic memory. Deterministic.
#include <stdio.h>

int main(void) {
  const char *s = "the quick brown fox jumps over the lazy dog";
  int words = 0, in_word = 0;
  for (int i = 0; s[i]; i++) {
    if (s[i] == ' ') {
      in_word = 0;
    } else if (!in_word) {
      in_word = 1;
      words++;
    }
  }
  printf("%d\n", words);
  return 0;
}
