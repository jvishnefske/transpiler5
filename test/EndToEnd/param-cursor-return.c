// REQUIRES: cargo
// FR-104 differential: returned cursors into a PARAMETER region,
// byte-diffed against the clang-built native binary (THE oracle for the
// relative-coordinate arithmetic). Seeds derive from argc so constant
// folding cannot hide a miscompile.
//
// lskip (verbatim inih ini_lskip shape, (char *) cast included): the
// callee walks a caller region and returns a cursor RELATIVE to its
// slice parameter; the caller re-slices its own region at
// argument-cursor + result. The adversarial cases this pins:
//   - the callee takes `const char *` but the CALLER writes through the
//     returned cursor (`*p = 'H'`) — C allows it, and it is sound in the
//     emitted Rust because the returned i64 carries no borrow (the
//     callee's borrow dies at the return);
//   - a second call at a NONZERO argument cursor (`lskip(p + 1)`), the
//     case that would miscompile if the caller forgot to add the
//     argument cursor back to the relative result.
// cpy (inih ini_strncpy0 shape): the rooted parameter is the FIRST of
// two slice parameters, pinning that the re-slice binds the region of
// the PROVEN parameter, not just "the first pointer argument".
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name param_cursor_return --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native 2>&1 > %t.native.out
// RUN: %t.crate/target/release/param_cursor_return > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

char *lskip(const char *s) {
  while (*s && *s <= ' ')
    s++;
  return (char *)s;
}

char *cpy(char *dest, const char *src, int size) {
  int i = 0;
  while (i < size - 1 && src[i]) {
    dest[i] = src[i];
    i = i + 1;
  }
  dest[i] = 0;
  return dest;
}

int main(int argc, char **argv) {
  char buf[16] = "   hello";
  /* argc-derived perturbation so nothing constant-folds */
  buf[1] = (char)(' ' + (argc - 1));
  char *p = lskip(buf);
  printf("first=%c idx=%d\n", *p, (int)(p - buf));
  /* caller writes through the returned cursor into its const-walked
     region */
  *p = 'H';
  printf("buf=%s\n", buf);
  /* call at a NONZERO cursor: relative-coordinate correctness */
  char *q = lskip(p + 1);
  printf("q=%c idx=%d\n", *q, (int)(q - buf));
  char dst[8];
  char *r = cpy(dst, q, 4 + (argc - 1));
  printf("dst=%s r0=%c\n", dst, *r);
  return 0;
}
