/* FR-161 companion TU: the DEFINITIONS whose bodies decide both the
   pointer model AND the element-zero fence. All three parameters here are
   subscripted or forwarded, so the importer classifies them as slices
   (`&mut [T]`) while the caller TU, which sees only a body-less prototype,
   passes `&mut T`. That divergence is what the merge reconciles.

   `setv` writes element 0 directly. `outer` never touches an element of
   its own parameter -- it hands the whole tail to `inner` -- so it is
   admitted only by the TRANSITIVE fence, and it is the systemd
   `parse_sec` -> `parse_time` shape in miniature. */

int setv(unsigned *p, unsigned v) {
  p[0] = v;
  return 0;
}

int inner(unsigned *r, unsigned v) {
  r[0] = v + 1u;
  return 0;
}

int outer(unsigned *ret, unsigned v) { return inner(ret, v); }
