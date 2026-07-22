// RealWorld corpus (Track 4): echo the command-line arguments. `argv` is a
// char** and `argv[i]` is a pointer-to-pointer subscript. Demand signal for
// C99-43 (ptr-to-ptr) and the argv-values-dropped-at-import limitation
// (emitrust-cc's main wrapper passes only argc).
#include <stdio.h>

int main(int argc, char **argv) {
  printf("%d\n", argc);
  for (int i = 0; i < argc; i++)
    printf("%s\n", argv[i]);
  return 0;
}
