// RealWorld corpus (Track 4): a grep-lite line scanner that binds strchr's
// RESULT to a pointer local (`const char *nl = strchr(...)`) and walks it.
// Demand signal for the strchr-result-bind blocker (strchr is only supported
// in fused, result-consuming positions today).
#include <stdio.h>
#include <string.h>

int main(void) {
  const char *text = "alpha\nbeta\ngamma\ndelta\n";
  const char *line = text;
  int matches = 0;
  while (line && *line) {
    const char *nl = strchr(line, '\n');
    if (strchr(line, 'a'))
      matches++;
    if (!nl)
      break;
    line = nl + 1;
  }
  printf("%d\n", matches);
  return 0;
}
