// REQUIRES: cargo
// FR-167 PHASE 2 differential end-to-end test, and the ONLY oracle that can
// see a miscompile in it: `cargo build` success is compile-only. Phase 2
// newly admits four union families as FR-78 opaque blobs that the one-slot
// model used to reject outright -- a POINTER arm, arms of DIFFERING SIZES,
// an aggregate arm over a scalar slot, and an UNNAMED (anonymous-struct)
// arm -- and every integer-scalar leaf of such a blob lowers through
// FR-83's byte views. So the property under test is the PUN PROPERTY on
// shapes that never had one before: a write through any arm must be
// readable, byte-exactly and at native endianness, through every
// overlapping leaf of every other arm, exactly as clang lays it out.
//
// The pointer arm is deliberately never READ or WRITTEN (it has no
// representation -- an access to it is a located rejection); what is
// pinned here is that its PRESENCE no longer kills the record, and that
// the record's other fields and the union's integer arms behave.
// FR-215's anonymous-union pointer arm (the 38.6x bucket) is the
// `struct Anon` case: phase 1's rollback re-routes it into phase 2's
// fallback, and its integer leaf must pun correctly through the blob.
//
// Seeds derive from argc so constant folding cannot hide a miscompile, and
// every byte read is written first through some arm (no uninitialized
// reads, no UB on either side). Every observable value is printed and
// byte-diffed against the clang-built native.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/union_opaque_fallback > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native 7 8 > %t.native3.out
// RUN: %t.crate/target/release/union_opaque_fallback 7 8 > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out
// RUN: %t.native a b c d e > %t.native6.out
// RUN: %t.crate/target/release/union_opaque_fallback a b c d e > %t.rust6.out
// RUN: diff %t.native6.out %t.rust6.out

int printf(const char *, ...);

/* Differing scalar widths: 8-byte blob, i overlaps the low 4 bytes of l. */
struct WidthMix {
  int tag;
  union {
    int i;
    long l;
  } u;
};

/* An aggregate arm over a SCALAR slot: FR-78's all-aggregate predicate
   missed this because the first arm is a scalar. 8-byte blob. */
struct Pair {
  int a;
  int b;
};

struct AggMix {
  int tag;
  union {
    int i;
    struct Pair n;
  } u;
};

/* A POINTER arm. Never accessed; its presence used to kill the record. */
struct PtrArm {
  int tag;
  union {
    int i;
    char *p;
  } u;
};

/* FR-215's shape: an ANONYMOUS union member with a pointer arm. */
struct AnonPtrArm {
  int tag;
  union {
    int i;
    char *p;
  };
};

/* An UNNAMED (anonymous-struct) arm of a named union. */
struct UnnamedArm {
  int tag;
  union {
    struct {
      int a;
      int b;
    };
    long l;
  } u;
};

struct WidthMix g_width;
struct AggMix g_agg;
struct PtrArm g_ptr;

void width_store_long(struct WidthMix *p, long v) { p->u.l = v; }
int width_load_int(struct WidthMix *p) { return p->u.i; }
void width_store_int(struct WidthMix *p, int v) { p->u.i = v; }
long width_load_long(struct WidthMix *p) { return p->u.l; }

void agg_store_pair(struct AggMix *p, int a, int b) {
  p->u.n.a = a;
  p->u.n.b = b;
}
int agg_load_int(struct AggMix *p) { return p->u.i; }

int main(int argc, char **argv) {
  unsigned seed = 2654435761u * (unsigned)argc;
  int lo = (int)(seed & 0x7fffffff);
  int hi = (int)((seed >> 3) & 0x7fffffff);
  long wide = ((long)hi << 32) | (long)(unsigned)lo;
  struct WidthMix w;
  struct AggMix a;
  struct PtrArm pa;
  struct AnonPtrArm ap;
  struct UnnamedArm un;
  struct WidthMix wcopy;
  int i;


  /* --- differing widths: write the WHOLE 8 bytes first, then pun. --- */
  w.tag = argc;
  width_store_long(&w, wide);
  printf("w.tag=%d w.l=%ld w.i=%d\n", w.tag, width_load_long(&w),
         width_load_int(&w));
  width_store_int(&w, lo ^ 0x5a5a5a5);
  printf("after i store: w.l=%ld w.i=%d\n", width_load_long(&w),
         width_load_int(&w));

  /* Dot access on a local, and a whole-record copy of a blob-bearing
     struct (the blob is a plain [u8; N], so the copy is a byte copy). */
  wcopy = w;
  wcopy.tag = w.tag + 1;
  printf("copy: tag=%d l=%ld i=%d\n", wcopy.tag, wcopy.u.l, wcopy.u.i);

  /* --- aggregate arm over a scalar slot --- */
  a.tag = -argc;
  agg_store_pair(&a, lo, hi);
  printf("a.tag=%d a.n.a=%d a.n.b=%d a.i=%d\n", a.tag, a.u.n.a, a.u.n.b,
         agg_load_int(&a));
  a.u.i = hi;
  printf("after i store: a.n.a=%d a.n.b=%d\n", a.u.n.a, a.u.n.b);

  /* --- pointer arm present but untouched --- */
  pa.tag = argc * 3;
  pa.u.i = lo;
  printf("pa.tag=%d pa.i=%d\n", pa.tag, pa.u.i);

  /* --- FR-215: anonymous union member with a pointer arm --- */
  ap.tag = argc * 5;
  ap.i = hi;
  printf("ap.tag=%d ap.i=%d\n", ap.tag, ap.i);

  /* --- unnamed (anonymous-struct) arm --- */
  un.tag = argc * 7;
  un.u.l = wide;
  printf("un.tag=%d un.u.l=%ld un.u.a=%d un.u.b=%d\n", un.tag, un.u.l, un.u.a, un.u.b);
  un.u.a = lo;
  un.u.b = hi;
  printf("after pair store: un.u.l=%ld\n", un.u.l);

  /* --- globals (zero-initialized), and a runtime-cursored loop --- */
  g_width.tag = argc;
  g_agg.tag = argc;
  g_ptr.tag = argc;
  for (i = 0; i < 4; i++) {
    g_width.u.l = wide + (long)i;
    g_agg.u.n.a = lo + i;
    g_agg.u.n.b = hi - i;
    printf("iter %d: g.i=%d g.l=%ld agg.i=%d agg.b=%d\n", i, g_width.u.i,
           g_width.u.l, g_agg.u.i, g_agg.u.n.b);
  }
  printf("g_ptr.tag=%d\n", g_ptr.tag);
  return 0;
}
