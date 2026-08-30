// REQUIRES: cargo
// FR-152 (defect): a place-backed local DECLARED INSIDE an unbounded loop
// body whose value ESCAPES the loop became an `!emitrust.lvalue<T>`-typed
// loop-carried result of `scf.while`, and SCFToEmitRust's WhileLowering has
// no default value for an lvalue type. Every function below was
// `error: failed to legalize operation 'scf.while' that was explicitly marked
// illegal` before the fix, so this whole file could not be emitted at all --
// which is why the shape is worth an EndToEnd test and not only a golden.
//
// The condition is type-independent, not "gperf-shaped": it fires for every
// local `emitLocalVar` routes to an `emitrust.variable` place (unsigned
// scalars, enums, structs, address-taken scalars, function pointers) and for
// every unbounded loop spelling (`for(;;)`, `do/while`, nesting), whether the
// value leaves through a `return` inside the loop or through a `break` and a
// use after it. All of those are covered here.
//
// The fix HOISTS the `emitrust.variable` op out of the loop so canonicalize
// can drop the now-invariant carried value. That is a real behaviour change:
// the place is no longer recreated per iteration, so a wrong hoist keeps a
// stale value alive across iterations instead of the fresh one. Only the
// stdout diff against the clang-built native binary can see that -- a
// successful `--build` cannot. Every value below derives from `argc`, so no
// constant folding can precompute an answer and hide a stale carry, and the
// file is run at several argc values so both sides of every in-loop branch
// are taken.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/loop_escaping_place > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a > %t.native2.out
// RUN: %t.crate/target/release/loop_escaping_place a > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/loop_escaping_place a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out
// RUN: %t.native a b c d e f g h i > %t.native10.out
// RUN: %t.crate/target/release/loop_escaping_place a b c d e f g h i > %t.rust10.out
// RUN: diff %t.native10.out %t.rust10.out

#include <stdio.h>

/* 1. The ten-line reproducer: an unsigned char place declared in a for(;;)
      body and returned from inside the loop. */
static int f_uchar(unsigned n) {
  for (;;) {
    unsigned char c = (unsigned char)(n * 3u + 1u);
    n += 37u;
    if (n > 400u)
      return (int)c;
  }
}

/* 2. An enum local: a place for the dialect-type reason, not the unsigned
      one, so it pins that the condition is not about integer signedness. */
enum Color { RED = 0, GREEN = 1, BLUE = 2 };

static int f_enum(unsigned n) {
  for (;;) {
    enum Color c = (enum Color)(n % 3u);
    n += 5u;
    if (n > 60u)
      return (int)c;
  }
}

/* 3. A struct local, written field by field inside the loop. */
struct P {
  int x;
  int y;
};

static int f_struct(unsigned n) {
  for (;;) {
    struct P p;
    p.x = (int)n;
    p.y = (int)(n * 3u);
    n += 7u;
    if (n > 70u)
      return p.x + p.y;
  }
}

/* 4. An address-taken signed scalar: place-backed because its address is
      passed to a callee, and mutated THROUGH that pointer, so a stale hoist
      would show up as the previous iteration's value. */
static void bump(int *p) { *p += 2; }

static int f_addr(unsigned n) {
  for (;;) {
    int c = (int)n;
    bump(&c);
    n += 6u;
    if (n > 65u)
      return c;
  }
}

/* 5. A function-pointer local, selected per iteration. */
static int add3(int v) { return v + 3; }
static int mul2(int v) { return v * 2; }

static int f_fnptr(unsigned n) {
  for (;;) {
    int (*op)(int);
    if (n & 1u)
      op = add3;
    else
      op = mul2;
    n += 3u;
    if (n > 33u)
      return op((int)n);
  }
}

/* 6. Escape through a `break` and a use AFTER the loop, not through a
      return from inside it. */
static int f_break(unsigned n) {
  unsigned char last = 200u;
  for (;;) {
    unsigned char c = (unsigned char)(n * 11u);
    n += 4u;
    if (n > 44u) {
      last = c;
      break;
    }
  }
  return (int)last;
}

/* 7. A do/while, whose lifted shape is the same scf.while. */
static int f_dowhile(unsigned n) {
  do {
    unsigned short c = (unsigned short)(n * 5u);
    n += 2u;
    if (c > 300u)
      return (int)c;
  } while (n < 1000u);
  return -1;
}

/* 8. Two nested unbounded loops: the inner place escapes through BOTH, so
      the hoist has to reach a fixpoint. */
static int f_nested(unsigned n) {
  for (;;) {
    for (;;) {
      unsigned short c = (unsigned short)(n * 3u);
      n += 9u;
      if (n > 90u)
        return (int)c;
    }
  }
}

/* 9. A place that is read after the exit test as well as inside the body,
      i.e. live in both the before- and the after-region. */
static int f_after_use(unsigned n) {
  int t = 0;
  for (;;) {
    unsigned char c = (unsigned char)n;
    if (n > 80u)
      return t + (int)c;
    t += (int)c * 2;
    n += 13u;
    t -= (int)c;
  }
}

/* 10. The gperf skeleton's gperf_case_strcmp verbatim -- the shape that
       first surfaced the defect -- over a table built at run time. */
static unsigned char gperf_downcase[256];

static void init_downcase(void) {
  int i;
  for (i = 0; i < 256; i++)
    gperf_downcase[i] = (i >= 'A' && i <= 'Z') ? (unsigned char)(i - 'A' + 'a')
                                               : (unsigned char)i;
}

static int gperf_case_strcmp(const char *s1, const char *s2) {
  for (;;) {
    unsigned char c1 = gperf_downcase[(unsigned char)*s1++];
    unsigned char c2 = gperf_downcase[(unsigned char)*s2++];
    if (c1 != 0 && c1 == c2)
      continue;
    return (int)c1 - (int)c2;
  }
}

/* The eight probe pairs are dispatched by an argc-derived index, so which
   pair runs first is not a compile-time constant. */
static int cmp_pair(unsigned k) {
  switch (k % 8u) {
  case 0:
    return gperf_case_strcmp("CAP_CHOWN", "cap_chown");
  case 1:
    return gperf_case_strcmp("abc", "abd");
  case 2:
    return gperf_case_strcmp("abd", "abc");
  case 3:
    return gperf_case_strcmp("", "");
  case 4:
    return gperf_case_strcmp("A", "");
  case 5:
    return gperf_case_strcmp("", "A");
  case 6:
    return gperf_case_strcmp("MixedCase", "mIXEDcASE");
  default:
    return gperf_case_strcmp("zzz", "zzzz");
  }
}

int main(int argc, char **argv) {
  unsigned n = (unsigned)argc; /* every value below derives from argc */
  unsigned i;

  init_downcase();

  printf("%d %d %d %d %d\n", f_uchar(n), f_enum(n), f_struct(n), f_addr(n),
         f_fnptr(n));
  printf("%d %d %d %d\n", f_break(n), f_dowhile(n), f_nested(n),
         f_after_use(n));

  for (i = 0; i < 8u; i++)
    printf("%d ", cmp_pair(i + n));
  printf("\n");

  /* Re-run the byte-shaped cases at a second, argc-derived seed so a hoist
     that leaks the previous CALL's value would also show. */
  printf("%d %d %d\n", f_uchar(n * 7u + 1u), f_break(n + 3u),
         f_after_use(n + 11u));
  return 0;
}
