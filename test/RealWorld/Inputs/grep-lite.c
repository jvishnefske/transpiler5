// RealWorld corpus (Track 4): a grep-lite line scanner that binds strchr's
// RESULT to a pointer local (`const char *nl = strchr(...)`) and walks it.
// The strchr bind itself is supported since FR-230 (the search is a param-0
// nullable cursor return); this file's demand signal MOVED FORWARD with it
// and is now `null pointer constant assigned to a pointer into a string
// literal` -- `line` is bound to a string literal region and the walk's
// null-termination assigns NULL into it.
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
