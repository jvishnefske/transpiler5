// Companion translation unit for pointers-param-invalid.c: the definition
// subscripts its pointer parameter, refining the signature to a slice
// shape the prototype-only TU could not anticipate.
int tally(int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s = s + a[i];
  }
  return s;
}
