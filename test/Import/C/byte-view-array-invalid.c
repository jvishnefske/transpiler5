// FR-229 Wave 2 negative space. Wave 2 admits three new shapes at a
// byte-slice argument -- a `memcpy` whose SOURCE is an object
// representation (capability C), the i8/u8 byte-array domain crossing
// (capability D), and the `(unsigned char *)&arr` spelling (capability E)
// -- and this file pins the boundary of each. The positives are
// byte-view-array.c; the Wave 1 boundary is byte-view-invalid.c.
//
// Each pin is here because crossing it would be a MISCOMPILE or a crate
// that does not build, never merely a missing feature:
//
// (1) A PADDED AGGREGATE AS A MEMCPY SOURCE. The `memcpy` path plans the
//     source image with the SAME planner the argument path uses, so the
//     measured indeterminate-padding refusal (`0100000007000000` on a
//     clean stack against `01dddddd07000000` after the frame is dirtied,
//     one binary, one value, two byte strings) covers it identically. This
//     matters more here than at the argument: `memcpy` WRITES those bytes
//     into a real array that the program then reads.
//
// (2) THE SYMMETRY PIN, BOTH AXES. Adding one padding byte to an admitted
//     memcpy-source struct must make it stop qualifying, and widening an
//     admitted `char[]` view to a non-byte element type (`short[]`) must
//     make it stop qualifying. Without a pin on the rejecting side of each
//     pair a later refactor could silently widen the admitted class and
//     nothing would fail.
//
// (3) A GLOBAL ARRAY BASE, both as a view and as a memcpy source. A
//     global's image would be built from a STAGED COPY, so a callee that
//     also touched the global directly would see -- or lose -- the wrong
//     bytes.
//
// (4) AN ESCAPING VIEW OF AN ARRAY. The whole admission premise is that
//     the view does not outlive the argument position; binding it to a
//     pointer is what would break that, and it is already refused. This
//     pin exists so a change to the byte view cannot quietly remove the
//     guard it depends on.
//
// (5) A WRITE THROUGH THE VIEW ON A CALL PATH WITH NO FLUSH POINT. The
//     i8 -> u8 view is a COPY, so a mutable parameter obliges a post-call
//     copy-out. The C++ method-call path has no flush point (Wave 1
//     confirmed RejectionLedger.cpp's note is literally true), so a view
//     there refuses instead of silently dropping the callee's writes.
//
// (6) A PARTIAL VIEW and a PARTIAL memcpy. `(unsigned char *)&raw[1]` and
//     `memcpy(raw, &x, 2)` name a WINDOW of the image, which this wave has
//     no lowering and no test for. They decline and keep their existing
//     located rejections rather than guessing at a windowed copy-out.
//
// (7) A DESTINATION TOO SMALL FOR THE IMAGE. Emitting it would produce
//     constant out-of-range array indices, which rustc rejects outright
//     (`unconditional_panic`) -- the silently-unbuildable class, which is
//     worse than a located refusal, not better.
//
// The last two RUN lines pin the LEDGER TAGS on the two Wave 1 wordings
// Wave 2 reuses. `RejectionLedger.cpp` and `run_realworld.py`'s
// `classify_blocker` are hand-mirrored by contract, and a wording that
// lands in the census's `other` junk bucket is invisible to anyone ranking
// work; reusing an already-mirrored wording is how Wave 2 adds no new
// junk-bucket rows at all.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/memcpy-padded.c 2>&1 | FileCheck %s --check-prefix=PADDED
// RUN: not emitrust-import-c %t/memcpy-symmetry.c 2>&1 | FileCheck %s --check-prefix=SYMPAD
// RUN: not emitrust-import-c %t/short-array.c 2>&1 | FileCheck %s --check-prefix=SYMWIDE
// RUN: not emitrust-import-c %t/global-array.c 2>&1 | FileCheck %s --check-prefix=GARRAY
// RUN: not emitrust-import-c %t/global-memcpy.c 2>&1 | FileCheck %s --check-prefix=GMEMCPY
// RUN: not emitrust-import-c %t/escape-array.c 2>&1 | FileCheck %s --check-prefix=ESCAPE
// RUN: not emitrust-import-c %t/partial-view.c 2>&1 | FileCheck %s --check-prefix=PARTIAL
// RUN: not emitrust-import-c %t/partial-memcpy.c 2>&1 | FileCheck %s --check-prefix=PMEMCPY
// RUN: not emitrust-import-c %t/short-dest.c 2>&1 | FileCheck %s --check-prefix=SHORTDST
// RUN: not emitrust-import-c %t/method-call.cpp 2>&1 | FileCheck %s --check-prefix=METHOD
// RUN: emitrust-cc --recover --emit=import %t/memcpy-padded.c -o /dev/null 2>&1 | FileCheck %s --check-prefix=TAGPAD
// RUN: emitrust-cc --recover --emit=import %t/global-array.c -o /dev/null 2>&1 | FileCheck %s --check-prefix=TAGGLOBAL

//--- memcpy-padded.c
// `{char; int;}` is 8 bytes with 1 + 4 == 5 bytes of members: three
// indeterminate padding bytes at offsets 1..3, and no value the emitted
// crate could write into them is right.
#include <stdio.h>
#include <string.h>
struct Q { char c; int i; };
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  struct Q q;
  unsigned char raw[8];
  q.c = 1;
  q.i = 7;
  memcpy(raw, &q, sizeof(q));
  ph(raw, sizeof(raw));
  return 0;
}
// PADDED: memcpy-padded.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view of an aggregate with interior padding

//--- memcpy-symmetry.c
// The admitted `{int; int;}` memcpy source with ONE byte added in the
// middle: size 12, members 4 + 1 + 4 == 9. It must STOP qualifying.
#include <stdio.h>
#include <string.h>
struct Pair { int a; char pad; int b; };
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  struct Pair p;
  unsigned char raw[12];
  p.a = 1;
  p.pad = 2;
  p.b = 3;
  memcpy(raw, &p, sizeof(p));
  ph(raw, sizeof(raw));
  return 0;
}
// SYMPAD: memcpy-symmetry.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view of an aggregate with interior padding

//--- short-array.c
// The other symmetry axis: `char a[4]` IS admitted as a byte view, and the
// same code over `short a[4]` is the object representation of an ARRAY --
// a different piece of work, and it must keep its refusal.
int printf(const char *, ...);
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  short a[4];
  a[0] = 1;
  a[1] = 2;
  a[2] = 3;
  a[3] = 4;
  ph((unsigned char *)&a, sizeof(a));
  return 0;
}
// SYMWIDE: short-array.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer expression: CStyleCastExpr

//--- global-array.c
int printf(const char *, ...);
char g[4] = {1, 2, 3, 4};
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  ph((unsigned char *)g, sizeof(g));
  return 0;
}
// GARRAY: global-array.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view of the global object 'g'

//--- global-memcpy.c
#include <stdio.h>
#include <string.h>
int gx = 7;
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  unsigned char raw[4];
  memcpy(raw, &gx, sizeof(gx));
  ph(raw, sizeof(raw));
  return 0;
}
// GMEMCPY: global-memcpy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view of the global object 'gx'

//--- escape-array.c
// The view bound to a pointer that outlives the call. The admission
// premise is that a byte view never escapes the argument position, and
// this refusal is what makes it hold for free.
int printf(const char *, ...);
int main(void) {
  char a[4];
  unsigned char *p;
  a[0] = 1;
  a[1] = 2;
  a[2] = 3;
  a[3] = 4;
  p = (unsigned char *)&a;
  printf("%d\n", (int)p[0]);
  return 0;
}
// ESCAPE: escape-array.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- partial-view.c
// A WINDOW of the array, not the array. The copy-out has to store back
// exactly the bytes the view covered, and this wave only has the
// whole-array form.
int printf(const char *, ...);
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  char a[4];
  a[0] = 1;
  a[1] = 2;
  a[2] = 3;
  a[3] = 4;
  ph((unsigned char *)&a[1], 2);
  return 0;
}
// PARTIAL: partial-view.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer expression: CStyleCastExpr

//--- partial-memcpy.c
// A count that is not the whole object image: the scatter has no windowed
// form, so it declines and the source resolution's own rejection stands.
#include <stdio.h>
#include <string.h>
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  int x = 7;
  unsigned char raw[4];
  raw[0] = 0;
  raw[1] = 0;
  raw[2] = 0;
  raw[3] = 0;
  memcpy(raw, &x, 2);
  ph(raw, sizeof(raw));
  return 0;
}
// PMEMCPY: partial-memcpy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object is not a string region

//--- short-dest.c
// The image does not fit. Emitting it would put constant out-of-range
// indices in the crate, which rustc rejects outright -- silently
// unbuildable is worse than located.
#include <stdio.h>
#include <string.h>
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  long long x = 7;
  unsigned char raw[4];
  memcpy(raw, &x, sizeof(x));
  ph(raw, sizeof(raw));
  return 0;
}
// SHORTDST: short-dest.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 8-byte access at offset 0 runs past the end of 'raw' (4 bytes)

//--- method-call.cpp
// A C++ method that writes through its byte-slice parameter. The
// method-call path has no post-call flush, so the i8 -> u8 view refuses
// rather than dropping the store into the copy and leaving `a` stale.
struct S {
  int v;
  void take(unsigned char *b, int n) {
    b[0] = 1;
    v = n;
  }
};
int printf(const char *, ...);
int main() {
  S s;
  char a[4];
  s.v = 0;
  a[0] = 1;
  a[1] = 2;
  a[2] = 3;
  a[3] = 4;
  s.take((unsigned char *)a, 4);
  printf("%d %d\n", s.v, (int)a[0]);
  return 0;
}
// METHOD: method-call.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view with no write-back point for the callee's writes

// Wave 2 adds NO new wording: both tags below are Wave 1 rows, already
// mirrored in `RejectionLedger.cpp` and `run_realworld.py`. `other` here
// would mean the mirrored tables drifted apart and the census can no
// longer rank this family.
// TAGPAD: blocker tabulation (recovered items by tag):
// TAGPAD-NEXT: byte-view-padding 1
// TAGGLOBAL: blocker tabulation (recovered items by tag):
// TAGGLOBAL-NEXT: byte-view-global 1
