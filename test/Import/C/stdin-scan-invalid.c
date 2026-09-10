// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/conv-long-double.c 2>&1 | FileCheck %s --check-prefix=LONGDOUBLE
// RUN: not emitrust-import-c %t/conv-lc.c 2>&1 | FileCheck %s --check-prefix=LENGTHLC
// RUN: not emitrust-import-c %t/type-float.c 2>&1 | FileCheck %s --check-prefix=TYPEFLOAT
// RUN: not emitrust-import-c %t/type-double.c 2>&1 | FileCheck %s --check-prefix=TYPEDOUBLE
// RUN: not emitrust-import-c %t/conv-string.c 2>&1 | FileCheck %s --check-prefix=STR
// RUN: not emitrust-import-c %t/conv-i.c 2>&1 | FileCheck %s --check-prefix=CONVI
// RUN: not emitrust-import-c %t/conv-x.c 2>&1 | FileCheck %s --check-prefix=CONVX
// RUN: not emitrust-import-c %t/conv-o.c 2>&1 | FileCheck %s --check-prefix=CONVO
// RUN: not emitrust-import-c %t/conv-p.c 2>&1 | FileCheck %s --check-prefix=CONVP
// RUN: not emitrust-import-c %t/conv-n.c 2>&1 | FileCheck %s --check-prefix=CONVN
// RUN: not emitrust-import-c %t/conv-set.c 2>&1 | FileCheck %s --check-prefix=CONVSET
// RUN: not emitrust-import-c %t/conv-percent.c 2>&1 | FileCheck %s --check-prefix=PERCENT
// RUN: not emitrust-import-c %t/conv-trailing.c 2>&1 | FileCheck %s --check-prefix=TRAILING
// RUN: not emitrust-import-c %t/width.c 2>&1 | FileCheck %s --check-prefix=WIDTH
// RUN: not emitrust-import-c %t/suppress.c 2>&1 | FileCheck %s --check-prefix=SUPPRESS
// RUN: not emitrust-import-c %t/length-l.c 2>&1 | FileCheck %s --check-prefix=LENGTHL
// RUN: not emitrust-import-c %t/length-h.c 2>&1 | FileCheck %s --check-prefix=LENGTHH
// RUN: not emitrust-import-c %t/nonliteral.c 2>&1 | FileCheck %s --check-prefix=NONLIT
// RUN: not emitrust-import-c %t/literal-match.c 2>&1 | FileCheck %s --check-prefix=LITMATCH
// RUN: not emitrust-import-c %t/count-mismatch.c 2>&1 | FileCheck %s --check-prefix=COUNT
// RUN: not emitrust-import-c %t/count-extra.c 2>&1 | FileCheck %s --check-prefix=EXTRA
// RUN: not emitrust-import-c %t/sscanf.c 2>&1 | FileCheck %s --check-prefix=SSCANF
// RUN: not emitrust-import-c %t/arg-element.c 2>&1 | FileCheck %s --check-prefix=ARGELEM
// RUN: not emitrust-import-c %t/arg-field.c 2>&1 | FileCheck %s --check-prefix=ARGFIELD
// RUN: not emitrust-import-c %t/arg-global.c 2>&1 | FileCheck %s --check-prefix=ARGGLOBAL
// RUN: not emitrust-import-c %t/arg-pointer.c 2>&1 | FileCheck %s --check-prefix=ARGPTR
// RUN: not emitrust-import-c %t/arg-cast.c 2>&1 | FileCheck %s --check-prefix=ARGCAST
// RUN: not emitrust-import-c %t/type-long.c 2>&1 | FileCheck %s --check-prefix=TYPELONG
// RUN: not emitrust-import-c %t/type-unsigned.c 2>&1 | FileCheck %s --check-prefix=TYPEU
// RUN: not emitrust-import-c %t/type-char.c 2>&1 | FileCheck %s --check-prefix=TYPEC
// RUN: not emitrust-import-c %t/fscanf-stream.c 2>&1 | FileCheck %s --check-prefix=FSCANFSTREAM

// FR-183 (standard input): the ADMITTED scanf grammar is deliberately tiny --
// a definition-less `scanf`, or `fscanf` on the literal `stdin`; an ordinary
// string-literal format of printable ASCII; directives exactly whitespace
// runs, `%d`, `%u`, `%c` and the float family `%a %e %f %g` (with an
// optional `l` selecting `double`), with NO other length modifier, no field
// width, no assignment-suppressing `*` and NO literal-match characters; one
// conversion per variadic argument; and every argument `&L` for a
// function-local scalar of exactly the matching type. Everything outside
// that is a LOCATED rejection, pinned below, because a scanf that silently
// diverged on malformed input is exactly the miscompile class this repo
// forbids.
//
// THE FLOAT PIN MOVED FORWARD, it did not loosen. FR-183 refused `%f`/`%lf`
// because glibc's float grammar accepts hex floats (`0x1p3` is 8) and
// `nan(1)`, which Rust's `parse` does not, and measured the conversion worth
// zero corpus cases at the time. Two corpus programs have since been
// measured to need it -- and they print the float's RAW BYTES, so only a
// bit-exact parse passes. The decimal/`inf`/`nan` subset IS bit-exact
// (both glibc's `strtof` and Rust's `f32` parser are correctly rounded), so
// it is now admitted; the two forms Rust cannot reproduce arrive from
// RUNTIME stdin and so cannot be refused here at all -- they are a loud
// runtime panic, pinned in `test/EndToEnd/stdin-scan-float.c`. What stays a
// COMPILE-TIME rejection is everything below: `%Lf` (long double is x87
// 80-bit and has no bit-exact Rust parser), every non-float use of a length
// modifier, and an argument whose type is not the conversion's own.
//
// `&arr[i]` must likewise STAY rejected: an element address routes through
// the FR-40 owner rewrite, which cannot apply to a `call_opaque` helper
// argument.
//
// The wordings below are authored by this RED phase and are binding on GREEN.

//--- conv-long-double.c
// `L` selects `long double`, which is x87 80-bit on this target: there is no
// Rust type that holds it, let alone a correctly-rounded parser for it, so
// it stays refused even though `%Lf` is spelled like the admitted family.
#include <stdio.h>
int main(void) {
  long double v = 0;
  scanf("%Lf", &v);
  return (int)v;
}
// LONGDOUBLE: conv-long-double.c:7:3: error: unsupported: scanf length modifier in '%Lf'

//--- conv-lc.c
// `l` is admitted ONLY as the float family's `double` selector; before any
// other conversion it is the same length-modifier rejection as before.
#include <stdio.h>
#include <wchar.h>
int main(void) {
  wchar_t w = 0;
  scanf("%lc", &w);
  return (int)w;
}
// LENGTHLC: conv-lc.c:7:3: error: unsupported: scanf length modifier in '%lc'

//--- conv-string.c
#include <stdio.h>
int main(void) {
  char b[8];
  scanf("%s", b);
  return b[0];
}
// STR: conv-string.c:4:3: error: unsupported: scanf conversion '%s' (only %d, %u, %c and %a/%e/%f/%g are supported)

//--- conv-i.c
#include <stdio.h>
int main(void) {
  int x = 0;
  scanf("%i", &x);
  return x;
}
// CONVI: conv-i.c:4:3: error: unsupported: scanf conversion '%i' (only %d, %u, %c and %a/%e/%f/%g are supported)

//--- conv-x.c
#include <stdio.h>
int main(void) {
  unsigned int x = 0;
  scanf("%x", &x);
  return (int)x;
}
// CONVX: conv-x.c:4:3: error: unsupported: scanf conversion '%x' (only %d, %u, %c and %a/%e/%f/%g are supported)

//--- conv-o.c
#include <stdio.h>
int main(void) {
  unsigned int x = 0;
  scanf("%o", &x);
  return (int)x;
}
// CONVO: conv-o.c:4:3: error: unsupported: scanf conversion '%o' (only %d, %u, %c and %a/%e/%f/%g are supported)

//--- conv-p.c
#include <stdio.h>
int main(void) {
  int x = 0;
  scanf("%p", &x);
  return x;
}
// CONVP: conv-p.c:4:3: error: unsupported: scanf conversion '%p' (only %d, %u, %c and %a/%e/%f/%g are supported)

//--- conv-n.c
#include <stdio.h>
int main(void) {
  int n = 0;
  scanf("%n", &n);
  return n;
}
// CONVN: conv-n.c:4:3: error: unsupported: scanf conversion '%n' (only %d, %u, %c and %a/%e/%f/%g are supported)

//--- conv-set.c
#include <stdio.h>
int main(void) {
  char b[8];
  scanf("%[abc]", b);
  return b[0];
}
// CONVSET: conv-set.c:4:3: error: unsupported: scanf conversion '%[' (only %d, %u, %c and %a/%e/%f/%g are supported)

//--- conv-percent.c
#include <stdio.h>
int main(void) {
  int x = 0;
  scanf("%%%d", &x);
  return x;
}
// PERCENT: conv-percent.c:4:3: error: unsupported: scanf conversion '%%' (only %d, %u, %c and %a/%e/%f/%g are supported)

//--- conv-trailing.c
#include <stdio.h>
int main(void) {
  int x = 0;
  scanf("%d%", &x);
  return x;
}
// TRAILING: conv-trailing.c:4:3: error: unsupported: trailing '%' in a scanf format

//--- width.c
#include <stdio.h>
int main(void) {
  int x = 0;
  scanf("%3d", &x);
  return x;
}
// WIDTH: width.c:4:3: error: unsupported: scanf field width in '%3d'

//--- suppress.c
#include <stdio.h>
int main(void) {
  int x = 0;
  scanf("%*d%d", &x);
  return x;
}
// SUPPRESS: suppress.c:4:3: error: unsupported: scanf assignment suppression in '%*d'

//--- length-l.c
#include <stdio.h>
int main(void) {
  long v = 0;
  scanf("%ld", &v);
  return (int)v;
}
// LENGTHL: length-l.c:4:3: error: unsupported: scanf length modifier in '%ld'

//--- length-h.c
#include <stdio.h>
int main(void) {
  short v = 0;
  scanf("%hd", &v);
  return v;
}
// LENGTHH: length-h.c:4:3: error: unsupported: scanf length modifier in '%hd'

//--- nonliteral.c
#include <stdio.h>
int main(void) {
  const char *fmt = "%d";
  int x = 0;
  scanf(fmt, &x);
  return x;
}
// NONLIT: nonliteral.c:5:3: error: unsupported: scanf format must be an ordinary string literal

//--- literal-match.c
#include <stdio.h>
int main(void) {
  int x = 0;
  scanf("x=%d", &x);
  return x;
}
// LITMATCH: literal-match.c:4:3: error: unsupported: literal-match character 'x' in a scanf format

//--- count-mismatch.c
#include <stdio.h>
int main(void) {
  int x = 0;
  scanf("%d %d", &x);
  return x;
}
// COUNT: count-mismatch.c:4:3: error: unsupported: scanf conversion count does not match the argument count

//--- count-extra.c
#include <stdio.h>
int main(void) {
  int x = 0;
  int y = 0;
  scanf("%d", &x, &y);
  return x + y;
}
// EXTRA: count-extra.c:5:3: error: unsupported: scanf conversion count does not match the argument count

//--- sscanf.c
#include <stdio.h>
int main(void) {
  char b[8];
  int x = 0;
  b[0] = 0;
  sscanf(b, "%d", &x);
  return x;
}
// SSCANF: sscanf.c:6:3: error: unsupported: 'sscanf' (only scanf and fscanf on stdin are supported)

//--- arg-element.c
#include <stdio.h>
int main(void) {
  int a[4];
  scanf("%d", &a[1]);
  return a[1];
}
// ARGELEM: arg-element.c:4:15: error: unsupported: a scanf argument must be the address of a function-local scalar variable

//--- arg-field.c
#include <stdio.h>
struct pt {
  int x;
};
int main(void) {
  struct pt p;
  scanf("%d", &p.x);
  return p.x;
}
// ARGFIELD: arg-field.c:7:15: error: unsupported: a scanf argument must be the address of a function-local scalar variable

//--- arg-global.c
#include <stdio.h>
int g;
int main(void) {
  scanf("%d", &g);
  return g;
}
// ARGGLOBAL: arg-global.c:4:15: error: unsupported: a scanf argument must be the address of a function-local scalar variable

//--- arg-pointer.c
#include <stdio.h>
int main(void) {
  int x = 0;
  int *p = &x;
  scanf("%d", p);
  return *p;
}
// ARGPTR: arg-pointer.c:5:15: error: unsupported: a scanf argument must be the address of a function-local scalar variable

//--- arg-cast.c
#include <stdio.h>
int main(void) {
  int x = 0;
  scanf("%d", (int *)&x);
  return x;
}
// ARGCAST: arg-cast.c:4:15: error: unsupported: a scanf argument must be the address of a function-local scalar variable

//--- type-long.c
#include <stdio.h>
int main(void) {
  long v = 0;
  scanf("%d", &v);
  return (int)v;
}
// TYPELONG: type-long.c:4:15: error: unsupported: scanf conversion '%d' requires an 'int' argument

//--- type-unsigned.c
#include <stdio.h>
int main(void) {
  int v = 0;
  scanf("%u", &v);
  return v;
}
// TYPEU: type-unsigned.c:4:15: error: unsupported: scanf conversion '%u' requires an 'unsigned int' argument

//--- type-char.c
#include <stdio.h>
int main(void) {
  int v = 0;
  scanf("%c", &v);
  return v;
}
// TYPEC: type-char.c:4:15: error: unsupported: scanf conversion '%c' requires a character argument

//--- type-float.c
// scanf is variadic, so no implicit conversion ever repairs a mismatch: a
// `double *` passed to `%f` would have glibc write four bytes into an eight
// byte object in C too.
#include <stdio.h>
int main(void) {
  double v = 0;
  scanf("%f", &v);
  return (int)v;
}
// TYPEFLOAT: type-float.c:7:15: error: unsupported: scanf conversion '%f' requires a 'float' argument

//--- type-double.c
#include <stdio.h>
int main(void) {
  float v = 0;
  scanf("%lf", &v);
  return (int)v;
}
// TYPEDOUBLE: type-double.c:4:16: error: unsupported: scanf conversion '%lf' requires a 'double' argument

//--- fscanf-stream.c
#include <stdio.h>
int main(void) {
  int x = 0;
  FILE *f = fopen("fr183_fscanf.txt", "r");
  fscanf(f, "%d", &x);
  fclose(f);
  return x;
}
// FSCANFSTREAM: fscanf-stream.c:5:10: error: unsupported: fscanf on a FILE* stream other than stdin
