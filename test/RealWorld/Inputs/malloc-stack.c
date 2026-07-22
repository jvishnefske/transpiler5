// RealWorld corpus (Track 4): a runtime-sized integer stack over a malloc'd
// buffer bound to a LOCAL pointer, then freed. Demand signal for C99-46.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int cap = 8;
  int *stack = malloc(cap * sizeof(int));
  int top = 0;
  for (int i = 0; i < 5; i++)
    stack[top++] = i * i;
  int sum = 0;
  while (top > 0)
    sum += stack[--top];
  printf("%d\n", sum);
  free(stack);
  return 0;
}
