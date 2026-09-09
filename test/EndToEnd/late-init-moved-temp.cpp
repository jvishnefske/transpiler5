// REQUIRES: cargo
// FR-132's DROP-ORDER GATE, narrowed -- and the narrowing byte-diffed.
//
// FR-132 refuses to merge `let x: T; .. x = v;` into `let x: T = v;` whenever
// ANY may-drop value is declared in the gap, because sinking a may-drop
// declaration past another one flips two destructors and BOTH orderings
// compile clean. That gate is deliberately coarse, and the value it most
// often refuses is the merged value ITSELF (`let v: T = f(); .. x = v;`).
// A gap value whose ONLY use is the merging assignment has no destructor
// left to reorder: it is either `Copy` -- and `Copy` and `Drop` are mutually
// exclusive in Rust, so it never had one -- or non-`Copy`, and then the
// assignment MOVES it and its scope-end drop is already a no-op. Both
// readings give the same answer, which is why the narrowing needs no move
// analysis and no liveness.
//
// THIS FILE IS THE VALUE ORACLE for that narrowing. A merge that dropped,
// duplicated or reordered an initializer compiles perfectly and prints the
// wrong number, so every case is diffed against the clang++-built native at
// THREE DISTINCT argument counts. Every value derives from `argc`, so nothing
// constant-folds and a lost store shows up as a wrong number rather than as a
// compile error -- and the three counts are 1, 3 and 7 rather than the
// `0`/`a`/`a b c` spelling, which supplies only TWO distinct seeds.
//
// `boxed_dtor` is the NEGATIVE leg and the one with an OBSERVABLE
// destructor. `std::make_unique<Node>` renders as a TWO-link chain
// (`let vA: Node = Node::default(); let vB: Box<Node> = Box::new(vA);`), so
// the gap holds a may-drop value (`vA`) whose use is the `Box::new` call and
// not the merging assignment. That stays REFUSED -- admitting it would need
// the move analysis this fence deliberately does not have -- and its
// interleaved `ctor`/`dtor` lines are what would expose a flipped destructor
// pair if the fence were ever widened without one.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.n1.out && %t.crate/target/release/late_init_moved_temp > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n3.out && %t.crate/target/release/late_init_moved_temp a b > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
// RUN: %t.native a b c d e f > %t.n7.out && %t.crate/target/release/late_init_moved_temp a b c d e f > %t.r7.out
// RUN: diff %t.n7.out %t.r7.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

#include <memory>
#include <optional>

extern "C" int printf(const char *, ...);

// A non-`Copy` struct (it has a user copy constructor) with NO destructor:
// the gate fires on it because `typeMayDrop` is deliberately WIDER than
// `has_drop`.
struct Tracer {
  int v;
  Tracer(int x) : v(x) {}
  Tracer(const Tracer &o) : v(o.v) {}
};

Tracer factory(int x) { return Tracer(x); }

// A payload with a PRINTING destructor -- the observable-drop leg.
struct Node {
  int id;
  Node(int i) : id(i) { printf("ctor %d\n", id); }
  ~Node() { printf("dtor %d\n", id); }
};

struct Pod {
  int a;
  int b;
};

// THE MOTIVATING SHAPE: the gap holds exactly the merged value. The
// assignment consumes `v`, so no destructor is left to reorder and the
// declaration may sink onto the write.
// CHECK-LABEL: fn moved_temp
// CHECK-NEXT:    let [[V:v[0-9]+]]: Tracer = factory(n + 3i32);
// CHECK-NEXT:    let t: Tracer = [[V]];
int moved_temp(int n) {
  Tracer t = factory(n + 3);
  printf("[moved %d]\n", t.v);
  return t.v * 2;
}

// The same narrowing through an opaque owner. `Box<i32>` is may-drop by the
// `OpaqueType` rule; `Box<Pod>` wraps a payload that is itself `Copy`, so the
// gap is still one link long even though the `Pod::default()` temporary sits
// in it.
// CHECK-LABEL: fn boxed_scalars
// CHECK:         let [[Z:v[0-9]+]]: Box<i32> = Box::new(0i32);
// CHECK-NEXT:    let z: Box<i32> = [[Z]];
// CHECK-NEXT:    let {{v[0-9]+}}: Pod = Pod::default();
// CHECK-NEXT:    let [[P:v[0-9]+]]: Box<Pod> = Box::new({{v[0-9]+}});
// CHECK-NEXT:    let mut p: Box<Pod> = [[P]];
int boxed_scalars(int n) {
  std::unique_ptr<int> e = std::make_unique<int>(n + 5);
  printf("[boxed %d]\n", *e);
  std::unique_ptr<int> z = std::make_unique<int>();
  std::unique_ptr<Pod> p = std::make_unique<Pod>();
  p->a = n + 7;
  p->b = *e + 1;
  return *e + *z + p->a + p->b;
}

std::optional<int> pick_even(int v) {
  if (v % 2 == 0)
    return v;
  return std::nullopt;
}

// `std::optional<int>` is may-drop by the same opaque rule and is `Copy` in
// the emitted Rust -- the leg that proves the narrowing does not secretly
// depend on the value being MOVED. `o` keeps a second read after the merge,
// which a move would have made a compile error.
// CHECK-LABEL: fn optional_temp
// CHECK:         let [[O:v[0-9]+]]: Option<i32> = None;
// CHECK-NEXT:    let o: Option<i32> = [[O]];
int optional_temp(int n) {
  std::optional<int> a = pick_even(n * 4);
  std::optional<int> o = std::nullopt;
  return a.value_or(-1) + o.value_or(n + 9) + (a.has_value() ? 1 : 0) +
         (o.has_value() ? 2 : 0);
}

// REFUSES, and this is the leg with a visible destructor. The gap is the two
// links `let vA: Node = Node::default(); let vB: Box<Node> = Box::new(vA);`,
// and `vA`'s use is the `Box::new` call, not the merging assignment. The bare
// declaration survives. The `ctor`/`dtor` interleaving in the diff is what
// would catch a flipped destructor pair if that ever changed.
// CHECK-LABEL: fn boxed_dtor
// CHECK-NEXT:    let {{(mut )?}}a: Box<Node>;
// CHECK:         Box::new
int boxed_dtor(int n) {
  std::unique_ptr<Node> a = std::make_unique<Node>(n + 11);
  printf("[dtor gap]\n");
  std::unique_ptr<Node> b = std::make_unique<Node>(n + 12);
  printf("[dtor gap 2]\n");
  return a->id + b->id;
}

int main(int argc, char **) {
  int n = argc;
  int r1 = moved_temp(n);
  int r2 = boxed_scalars(n);
  int r3 = optional_temp(n);
  int r4 = boxed_dtor(n);
  printf("%d %d %d %d\n", r1, r2, r3, r4);
  return 0;
}
