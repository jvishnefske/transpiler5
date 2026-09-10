// FR-229 Wave 1 negative space. The scalar and padding-free-aggregate byte
// view is admitted at a byte-slice ARGUMENT (see byte-view.c); this file
// pins the boundary of that admission, and each pin is here because
// crossing it would be a MISCOMPILE rather than a build failure.
//
// (1) A PADDED AGGREGATE. C leaves padding bytes indeterminate, and this is
//     measured, not argued: one program, one clang -O0 binary, one run,
//     the same `{char; int;}` value, two different byte strings --
//     `0100000007000000` on a clean stack and `01dddddd07000000` after the
//     frame is dirtied. There is no value the emitted crate could scatter
//     into those holes that is right, and the byte-diff oracle cannot see
//     the error because every harness initializes every field (FR-212's
//     uninitialized-memory blind spot). Admission is therefore gated on
//     clang's OWN `emitrust.abi_layout` numbers -- `size == sum(field
//     sizes)` with contiguous offsets -- never on a layout re-derived here.
//
// (2) THE SYMMETRY PIN. `pair_t {int; int;}` IS admitted; the same struct
//     with one extra `char` between the fields is NOT. Without a pin on the
//     rejecting side of that pair a later refactor could silently widen the
//     admitted class and nothing would fail.
//
// (3) A GLOBAL BASE. This MOVES the `byte-region-aggregates-invalid.c`
//     int-leaf pin forward rather than loosening it: the object
//     representation of a global would have to be built from a STAGED COPY,
//     so a callee that also touches the global directly would see or lose
//     the wrong values. Still one located rejection, now naming the global.
//
// (4) AN ESCAPING VIEW. `unsigned char *p = (unsigned char *)&x;` binds the
//     byte image to a pointer that outlives the call, and the whole
//     admission premise is that the view does NOT escape the argument
//     position. It is already refused as `pointer assigned a non-address
//     value`, and that guard is what makes the non-escape premise hold for
//     free -- this pin exists so a change to the byte view cannot quietly
//     remove it.
//
// (5) `(unsigned char *)&arr` OVER A NON-BYTE ELEMENT TYPE. Wave 2's
//     capability E admits the `&arr` spelling for a BYTE array (see
//     byte-view-array.c), and `array-base.c` below is the rejecting side
//     of that pin: `int a[3]` is the object representation of an ARRAY,
//     which is a different piece of work from a byte array's domain
//     crossing, and it must keep its refusal verbatim. Widening an
//     admitted `char[]` to a non-byte element type has to make the shape
//     STOP qualifying, and this is where that is checked.
//
// (6) A MUTABLE view on a call path with NO post-call write-back point.
//     The C++ method-call path is that path -- it builds its call op
//     somewhere the argument materializer cannot flush into, which is the
//     same gap RejectionLedger.cpp records for mutating methods through a
//     pointer to a global. A byte view there would lose the callee's
//     writes silently, so it refuses instead.
//
// The last three RUN lines pin the LEDGER TAGS. `RejectionLedger.cpp` and
// `run_realworld.py`'s `classify_blocker` are hand-mirrored by contract,
// and a wording with no row in both lands in the census's `other` junk
// bucket where nobody ranking work will ever see it. These are the pins
// that make that a test failure rather than an invisible regression.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/padded.c 2>&1 | FileCheck %s --check-prefix=PADDED
// RUN: not emitrust-import-c %t/symmetry.c 2>&1 | FileCheck %s --check-prefix=SYMMETRY
// RUN: not emitrust-import-c %t/global-scalar.c 2>&1 | FileCheck %s --check-prefix=GSCALAR
// RUN: not emitrust-import-c %t/global-aggregate.c 2>&1 | FileCheck %s --check-prefix=GAGG
// RUN: not emitrust-import-c %t/escape.c 2>&1 | FileCheck %s --check-prefix=ESCAPE
// RUN: not emitrust-import-c %t/array-base.c 2>&1 | FileCheck %s --check-prefix=ARRAY
// RUN: not emitrust-import-c %t/nested-member.c 2>&1 | FileCheck %s --check-prefix=NESTED
// RUN: not emitrust-import-c %t/method-call.cpp 2>&1 | FileCheck %s --check-prefix=METHOD
// RUN: emitrust-cc --recover --emit=import %t/padded.c -o /dev/null 2>&1 | FileCheck %s --check-prefix=TAGPAD
// RUN: emitrust-cc --recover --emit=import %t/global-scalar.c -o /dev/null 2>&1 | FileCheck %s --check-prefix=TAGGLOBAL
// RUN: emitrust-cc --recover --emit=import %t/method-call.cpp -o /dev/null 2>&1 | FileCheck %s --check-prefix=TAGWB

//--- padded.c
// `{char; int;}` is 8 bytes with 1 + 4 == 5 bytes of members: three
// indeterminate padding bytes at offsets 1..3.
int printf(const char *, ...);
struct Q { char c; int i; };
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  struct Q q;
  q.c = 1;
  q.i = 7;
  ph((unsigned char *)&q, sizeof(q));
  return 0;
}
// PADDED: padded.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view of an aggregate with interior padding

//--- symmetry.c
// The admitted `{int; int;}` with ONE byte added in the middle: size 12,
// members 4 + 1 + 4 == 9. It must STOP qualifying.
int printf(const char *, ...);
struct Pair { int a; char pad; int b; };
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  struct Pair p;
  p.a = 1;
  p.pad = 2;
  p.b = 3;
  ph((unsigned char *)&p, sizeof(p));
  return 0;
}
// SYMMETRY: symmetry.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view of an aggregate with interior padding

//--- global-scalar.c
int printf(const char *, ...);
int gx = 7;
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  ph((unsigned char *)&gx, sizeof(gx));
  return 0;
}
// GSCALAR: global-scalar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view of the global object 'gx'

//--- global-aggregate.c
// The padding-free int-leaf struct of byte-region-aggregates-invalid.c,
// walked through a `*p++` cursor. Both axes that separate it from the
// admitted local shape are present; the GLOBAL one is what is named.
typedef unsigned char u8;
struct P { int x; int y; };
struct P gp = {1, 2};
int printf(const char *, ...);
void print_(const u8 *p, long size) {
  while (size--)
    printf(" %x", *p++);
}
int main(void) {
  print_((u8 *)&gp, sizeof gp);
  return 0;
}
// GAGG: global-aggregate.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view of the global object 'gp'

//--- escape.c
int printf(const char *, ...);
int main(void) {
  int x = 7;
  unsigned char *p = (unsigned char *)&x;
  printf("%d\n", (int)p[0]);
  return 0;
}
// ESCAPE: escape.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- array-base.c
int printf(const char *, ...);
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  int a[3] = {1, 2, 3};
  ph((unsigned char *)&a, sizeof(a));
  return 0;
}
// ARRAY: array-base.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer expression: CStyleCastExpr

//--- nested-member.c
// Padding-free and int-leaved, but a member is itself an aggregate: the
// Wave 1 scatter is per SCALAR field, so this keeps the standing
// non-byte-members refusal rather than guessing at a recursive offset map.
int printf(const char *, ...);
struct Inner { int a; int b; };
struct Outer { struct Inner in; int z; };
static void ph(unsigned char *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    printf("%02x", p[i]);
}
int main(void) {
  struct Outer o;
  o.in.a = 1;
  o.in.b = 2;
  o.z = 3;
  ph((unsigned char *)&o, sizeof(o));
  return 0;
}
// NESTED: nested-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view of an aggregate with non-byte members

//--- method-call.cpp
// A C++ method that writes through its byte-slice parameter. The
// method-call path has no post-call write-back flush, so the view refuses
// rather than dropping the store: `s.v` would be right and `x` silently
// stale.
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
  s.v = 0;
  int x = 3;
  s.take((unsigned char *)&x, 4);
  printf("%d %d\n", s.v, x);
  return 0;
}
// METHOD: method-call.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view with no write-back point for the callee's writes

// The three tags, one per wording. `other` would mean the mirrored tables
// drifted apart and the census can no longer rank this family.
// TAGPAD: blocker tabulation (recovered items by tag):
// TAGPAD-NEXT: byte-view-padding 1
// TAGGLOBAL: blocker tabulation (recovered items by tag):
// TAGGLOBAL-NEXT: byte-view-global 1
// TAGWB: blocker tabulation (recovered items by tag):
// TAGWB-NEXT: byte-view-writeback 1
