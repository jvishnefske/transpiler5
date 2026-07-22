// RealWorld corpus (Track 4): Sieve of Eratosthenes over a fixed bound.
// Array-based, deterministic stdout, no dynamic memory — a supported-subset
// algorithm that doubles as a runtime perf workload.
#include <stdio.h>

#define N 50

int main(void) {
  int composite[N];
  for (int i = 0; i < N; i++)
    composite[i] = 0;
  for (int i = 2; i < N; i++) {
    if (!composite[i]) {
      printf("%d ", i);
      for (int j = i * i; j < N; j += i)
        composite[j] = 1;
    }
  }
  printf("\n");
  return 0;
}
