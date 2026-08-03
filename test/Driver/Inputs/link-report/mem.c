// Report-test TU exercising three distinct blocker tags: dynamic-memory
// (non-constant malloc), other (void-pointer parameter), and libc:qsort
// (system-header call).

#include <stdlib.h>

int use_malloc(int n) {
  int *p = malloc(n * sizeof(int));
  p[0] = 1;
  free(p);
  return 1;
}

int use_qsort_cmp(const void *a, const void *b);

int sorts(int *a) {
  qsort(a, 4, sizeof(int), use_qsort_cmp);
  return a[0];
}
