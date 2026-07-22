// RealWorld corpus (Track 4): recursive-descent integer expression evaluator
// over a fixed input string, driven by a global cursor into a string literal.
// Exercises a reassigned/walked global char* cursor + mutual recursion.
#include <stdio.h>

static const char *p;

static int expr(void);

static int number(void) {
  int v = 0;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    p++;
  }
  return v;
}

static int factor(void) {
  if (*p == '(') {
    p++;
    int v = expr();
    p++; /* ')' */
    return v;
  }
  return number();
}

static int term(void) {
  int v = factor();
  while (*p == '*' || *p == '/') {
    char op = *p++;
    int r = factor();
    v = (op == '*') ? v * r : v / r;
  }
  return v;
}

static int expr(void) {
  int v = term();
  while (*p == '+' || *p == '-') {
    char op = *p++;
    int r = term();
    v = (op == '+') ? v + r : v - r;
  }
  return v;
}

int main(void) {
  p = "2+3*4-(1+1)";
  printf("%d\n", expr());
  return 0;
}
