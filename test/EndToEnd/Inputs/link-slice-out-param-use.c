/* FR-161 companion TU: the CALLER. Every prototype is body-less, so this
   TU's solo import shapes `setv` and `outer` from the declaration alone and
   passes `&mut T` at each call site. The four out-parameters cover the
   measured systemd census split: a scalar LOCAL (50 of 60 slots) and a
   struct FIELD (the other 10), each written through by the callee.

   `guard` and `s.b` are the fidelity witnesses. C's `&x` lends a
   one-element region and the wrap makes that exact; a wrap that widened
   the borrow -- or a callee that walked past element 0 -- would show up
   here as a changed neighbour, and the byte-diff against the native would
   catch it. */

int printf(const char *, ...);

struct S {
  unsigned a;
  unsigned b;
};

int setv(unsigned *, unsigned);
int outer(unsigned *, unsigned);

int run(int k) {
  unsigned x = 99;
  unsigned y = 0;
  unsigned guard = 5;
  struct S s;
  s.a = 7;
  s.b = 8;
  setv(&x, 11u + (unsigned)k);
  setv(&s.a, 22u + (unsigned)k);
  outer(&y, 33u + (unsigned)k);
  printf("%u %u %u %u %u\n", x, s.a, s.b, y, guard);
  return (int)(x + s.a + s.b + y + guard);
}
