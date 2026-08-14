// FR-62 slice 4 (stage A): `--actor-lift --emit=rust` — pins the EMITTED
// readability contract of the lift, the reason the owner wants default-on:
// the arm renders as the SLICE-4 SPIKE's two-line method body (member
// assign + tail expression, no thread_local `.with(...)` snapshot
// ceremony), the struct derives the standard set, the receiver is a plain
// `&mut self`, main constructs the actor as a defaulted named local with
// the non-default field initializer as a post-construction assign, the
// main-only global becomes a named local carrying its C initializer, and
// no `thread_local!` block survives anywhere in the crate root
// (--implicit-check-not).
// RUN: emitrust-cc --actor-lift --emit=rust %s -o - \
// RUN:   | FileCheck %s --implicit-check-not=thread_local

int printf(const char *, ...);

int counter;
const int scale = 3;
int limit = 50;

int bump(void) {
  counter += scale;
  return counter;
}

int tally(int x) {
  static int total = 7;
  total += x;
  return total;
}

int main(void) {
  int b = bump();
  int t = tally(2);
  limit = limit + b + t;
  printf("b=%d t=%d limit=%d\n", b, t, limit);
  return 0;
}

// CHECK:      #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct CounterActor {
// CHECK-NEXT:     counter: i32,
// CHECK-NEXT: }
// CHECK:      impl CounterActor {
// CHECK-NEXT:     fn bump(&mut self) -> i32 {
// CHECK-NEXT:         self.counter += SCALE;
// CHECK-NEXT:         self.counter
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK:      struct TallyActor {
// CHECK-NEXT:     total: i32,
// CHECK-NEXT: }
// CHECK:      impl TallyActor {
// CHECK-NEXT:     fn tally(&mut self, x: i32) -> i32 {
// CHECK-NEXT:         self.total += x;
// CHECK-NEXT:         self.total
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK:      fn c_main() -> i32 {
// CHECK-NEXT:     let mut counter_actor: CounterActor = CounterActor::default();
// CHECK-NEXT:     let mut tally_actor: TallyActor = TallyActor { total: 7i32, };
// CHECK-NEXT:     let mut limit: i32 = 50;
// CHECK:          counter_actor.bump()
// CHECK:          tally_actor.tally(2i32)
// CHECK:          println!("b={} t={} limit={}"
