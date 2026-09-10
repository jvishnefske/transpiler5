// FR-140 byte-identity guard: a program whose emitted names all pass
// rustc's `is_snake_case` must emit EXACTLY what it emitted before FR-140 --
// no `#[allow(non_snake_case)]`, no shifted byte, no shifted line.
//
// FR-140 adds a per-item allow attribute for names carrying an interior
// double underscore. `--emit=crate` output is pinned byte-for-byte by
// golden tests, so the attribute must appear ONLY where a name genuinely
// trips: this file is the negative half of
// test/Driver/double-underscore-names.c and covers the shapes that make
// the predicate easy to get wrong. rustc's `is_snake_case` IGNORES leading
// and trailing underscores before looking for a doubled run, so the field
// `a__` (trailing) and the field `_lead` (leading) are both perfectly
// snake_case and must stay bare; a single interior underscore (`mid_dle`,
// `step_x`) never trips at all. The whole crate root is pinned line by
// line under --strict-whitespace so a stray attribute anywhere -- on the
// struct, the impl, the fn, or the let -- fails this test.
//
// RUN: emitrust-cc --emit=rust %s -o - \
// RUN:   | FileCheck %s --strict-whitespace --match-full-lines
//
// The attribute must not appear ANYWHERE in the crate root; the full-line
// pin above cannot say that on its own (a CHECK-NOT only guards the span
// between its neighbours), so a second, unanchored scan says it directly.
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s --check-prefix=NOATTR

extern int printf(const char *, ...);

int g_count = 1;

struct pair_t { int a__; int _lead; int mid_dle; };

int bump(int step_x) { int acc_um = step_x + g_count; return acc_um * 2; }

int main(void) {
  struct pair_t p_v;
  p_v.a__ = bump(3);
  p_v._lead = p_v.a__ - 1;
  p_v.mid_dle = p_v._lead + g_count;
  printf("%d %d %d\n", p_v.a__, p_v._lead, p_v.mid_dle);
  return 0;
}

// FR-220 deleted the crate root's blanket `#![allow(dead_code)]` and the blank
// line that followed it, and put a targeted `#[allow(dead_code)]` on each
// record and each INHERENT impl instead. What moved: two header lines gone
// from the top, three item-level attributes added. Nothing about
// `non_snake_case` changed, which is the invariant this file exists to pin --
// the NOATTR sweep below is untouched and still says the FR-140 attribute
// appears nowhere.
// CHECK:#[allow(dead_code)]
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct GCountActor {
// CHECK-NEXT:    g_count: i32,
// CHECK-NEXT:}
// CHECK-NEXT:#[allow(dead_code)]
// CHECK-NEXT:impl GCountActor {
// CHECK-NEXT:    fn bump(&mut self, step_x: i32) -> i32 {
// CHECK-NEXT:        (step_x + self.g_count) * 2i32
// CHECK-NEXT:    }
// CHECK-NEXT:}
// CHECK-NEXT:#[allow(dead_code)]
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct PairT {
// CHECK-NEXT:    a__: i32,
// CHECK-NEXT:    _lead: i32,
// CHECK-NEXT:    mid_dle: i32,
// CHECK-NEXT:}
// CHECK-NEXT:fn c_main() -> i32 {
// CHECK-NEXT:    let mut g_count_actor: GCountActor = GCountActor { g_count: 1i32, };
// CHECK-NEXT:    let mut p_v: PairT = PairT::default();
// CHECK-NEXT:    let v4: i32 = g_count_actor.bump(3i32);
// CHECK-NEXT:    p_v.a__ = v4;
// CHECK-NEXT:    p_v._lead = p_v.a__ - 1i32;
// CHECK-NEXT:    p_v.mid_dle = p_v._lead + g_count_actor.g_count;
// CHECK-NEXT:    println!("{} {} {}", p_v.a__, p_v._lead, p_v.mid_dle);
// CHECK-NEXT:    0i32
// CHECK-NEXT:}
// CHECK-EMPTY:
// CHECK-NEXT:fn main() { std::process::exit(c_main()); }
// NOATTR-NOT: allow(non_snake_case)
