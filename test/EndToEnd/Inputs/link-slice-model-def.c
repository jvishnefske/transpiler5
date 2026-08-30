/* FR-158 companion TU: the DEFINITIONS whose bodies decide the pointer
   model. `emit` and `isum` subscript their pointer parameters, so the
   importer classifies them as slices (`&mut [T]`); `peek` only
   dereferences its parameter, so it stays a SCALAR reference (`&mut T`).
   The pair is what makes link-slice-model-e2e.c's use TU carry BOTH a
   diverging and a non-diverging obligation over the same base. */

int putchar(int);

void emit(char *s, int n) {
  for (int i = 0; i < n; i++)
    putchar(s[i]);
  putchar('\n');
}

int isum(int *a, int n) {
  int t = 0;
  for (int i = 0; i < n; i++)
    t += a[i];
  return t;
}

int peek(char *p) { return *p; }
