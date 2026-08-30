/* FR-158 companion TU: the CALLER. Every prototype here is body-less, so
   this TU's solo import shapes `emit`/`isum`/`peek` from the bare
   declaration and passes `&mut T` at each call. The defining TU refines
   `emit` and `isum` to `&mut [T]`; `peek` stays scalar. `fwd` is the
   forwarded-parameter base: its own `p` is slice-classified here (this
   body subscripts it), so the reconciled argument is built over a DEREF
   of a reference parameter rather than a local array. */

void emit(char *s, int n);
int isum(int *a, int n);
int peek(char *p);

int run(int k) {
  char buf[8] = "abcdefg";
  int nums[5] = {1, 2, 3, 4, 5};
  emit(&buf[k], 3);
  emit(&buf[0], 2);
  /* Same base, same index, two callees: only `isum`'s argument is
     slice-refined, so `peek`'s scalar borrow must survive untouched. */
  return isum(&nums[k], 2) + peek(&buf[k]);
}

int fwd(char *p, int k) {
  emit(&p[k], 2);
  return 0;
}
