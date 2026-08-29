// REQUIRES: cargo
// FR-150 differential end-to-end test: a C type named `Option` shadows Rust's
// prelude `Option`, and the crate must still BUILD and behave identically.
//
// This is the systemd defect verbatim. `src/shared/options.h:45` ends
// `} Option;`, so the importer emits `pub struct Option { .. }`; from that
// point every `Option<fn(..)>` the emitter writes for a function pointer
// parses as THAT struct, and the crate dies with rustc E0107 ("struct takes 0
// generic arguments but 1 generic argument was supplied") and E0599 ("no
// method named `expect` found for struct `Option`"). emitrust-cc exits 0 and
// prints nothing: silent unbuildable output, the FR-140/141/142/146 class.
// 123 of the 158 systemd build failures were this one collision.
//
// The oracle here is the BYTE-DIFF against the clang-built native, not the
// build. `cargo build` succeeding after qualification would only prove the
// crate parses; it cannot see a dispatch that qualification got wrong. So
// every seed, every branch and every callback argument derives from argc, in
// three argument counts, and the program has no UB.
//
// FR-133 MATTERS HERE and is why this file avoids the fn-ptr LOCAL shape:
// FR-133 drops the `Option` wrapper for a local initialized from a literal
// `Some`, so a repro built only out of locals BUILDS CLEAN today and would
// pin nothing. The collision now bites exactly at the sites FR-133 left
// wrapped, and all three are exercised below:
//   * a fn-ptr PARAMETER (`apply`), including its `.expect` unwrap,
//   * a fn-ptr STRUCT MEMBER (`struct ops`), dispatched and rebound,
//   * a fn-ptr GLOBAL (`g_hook`), rebound at run time from argc.
// `Option` itself is passed by value, returned, and read back so that a
// qualification that accidentally rewrote the USER type would change the
// program's answers rather than merely its bytes.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native0.out
// RUN: %t.crate/target/release/prelude_shadow_option > %t.rust0.out
// RUN: diff %t.native0.out %t.rust0.out
// RUN: %t.native one > %t.native1.out
// RUN: %t.crate/target/release/prelude_shadow_option one > %t.rust1.out
// RUN: diff %t.native1.out %t.rust1.out
// RUN: %t.native one two three > %t.native3.out
// RUN: %t.crate/target/release/prelude_shadow_option one two three > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

// The colliding type, spelled exactly as systemd's options.h spells it.
typedef struct Option {
  int id;
  int flags;
} Option;

struct ops {
  int (*apply)(int);
  int tag;
};

static int inc(int x) { return x + 1; }
static int dbl(int x) { return x * 2; }
static int neg(int x) { return -x - 1; }

// A fn-ptr PARAMETER: `Option<fn(i32) -> i32>` plus `.expect(..)`.
static int apply(int (*h)(int), int v) { return h(v); }

// The colliding struct crosses a function boundary by value and comes back,
// so a rewrite of the USER type would be observable in the output.
static Option bump(Option o, int by) {
  o.id += by;
  o.flags *= 2;
  return o;
}

// A fn-ptr GLOBAL, rebound at run time.
static int (*g_hook)(int) = inc;

int main(int argc, char **argv) {
  Option o;
  o.id = argc * 3;
  o.flags = argc + 5;

  // fn-ptr PARAMETER, with an argc-selected callee so nothing folds. `sel`
  // is written on three arms and then ESCAPES as a call argument, so FR-133
  // refuses to unwrap it and the local keeps its `Option` too.
  int (*sel)(int);
  if (argc > 2)
    sel = neg;
  else if (argc > 1)
    sel = dbl;
  else
    sel = inc;
  int a = apply(sel, o.id);
  printf("a=%d id=%d flags=%d\n", a, o.id, o.flags);

  // fn-ptr STRUCT MEMBER: assigned, dispatched, rebound, dispatched again.
  struct ops s;
  if (argc > 1) {
    s.apply = dbl;
    s.tag = 7;
  } else {
    s.apply = inc;
    s.tag = 9;
  }
  int b = s.apply(o.id) + s.tag;
  s.apply = neg;
  int c = s.apply(b);
  printf("b=%d c=%d tag=%d\n", b, c, s.tag);

  // fn-ptr GLOBAL, rebound from argc.
  if (argc > 2)
    g_hook = neg;
  else if (argc > 1)
    g_hook = dbl;
  int d = g_hook(o.flags) + g_hook(argc);
  printf("d=%d\n", d);

  // The user's own `Option` by value, through a call and back.
  Option p = bump(o, argc);
  printf("p.id=%d p.flags=%d o.id=%d o.flags=%d\n", p.id, p.flags, o.id,
         o.flags);
  return 0;
}
