// Defining TU for link-soletu-owners.c: an externally visible function
// whose pointer parameter's model is a property of THIS body (slice), plus
// a caller that passes a pointer into an internal global -- the shape whose
// cell-slice promotion is legal ONLY under a whole-program claim no shard
// may make.

static int A[4];

int sum4(int *a) {
  int s = 0;
  int i;
  for (i = 0; i < 4; i++)
    s = s + a[i];
  return s;
}

int fill_and_sum(void) {
  int i;
  for (i = 0; i < 4; i++)
    A[i] = i + 1;
  return sum4(A);
}
