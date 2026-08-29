// REQUIRES: cargo
// FR-150, the `Box` leg, byte-diffed against clang++ -std=c++17. W2.21 maps
// `std::unique_ptr<T>` to the opaque `Box<T>` and `std::make_unique` to
// `Box::new`, so a C++ class named `box` shadows the prelude name in exactly
// the crate that needs it: the pre-fix emission dies with rustc E0107 on
// `Box<Node>`, E0599 ("no associated function named `new` found for struct
// `Box`") on `Box::new`, and E0277 ("the trait bound `Box: Deref` is not
// satisfied") on every payload access -- again exit 0 with no diagnostic.
//
// `toUpperCamelCase` drops underscores, so `box` collides and `my_box` does
// not; both live in this crate.
//
// The `Box` spellings reached: the local's `let p: Box<Node>` annotation, the
// `Box::new(..)` construction, and the Deref/DerefMut borrows behind `p->f`.
// `Node::default()` sits right beside `Box::new` on the SAME emission path
// (both are `emitrust.call_opaque` callees), which is why a blanket rewrite of
// call callees would break the user's own static method -- the diff catches
// that, a build alone would not.
//
// DROP TIMING is the reason a printf destructor is here: a Box drops its
// payload, so the C++ destructor and the Rust `Drop` must fire at the same
// program point. The boxes sit in a loop body and at function scope with
// output after the last use. Every value derives from argc.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native0.out
// RUN: %t.crate/target/release/prelude_shadow_box > %t.rust0.out
// RUN: diff %t.native0.out %t.rust0.out
// RUN: %t.native one two > %t.native2.out
// RUN: %t.crate/target/release/prelude_shadow_box one two > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out

extern "C" int printf(const char *, ...);

#include <memory>

// The colliding type. `box` -> `Box` under toUpperCamelCase.
struct box {
  int w;
  int h;
  int area() const { return w * h; }
};

// The underscore twin, which does NOT collide (`MyBox`).
struct my_box {
  int d;
};

struct Node {
  int id;
  int tag;
  Node(int i, int t) : id(i), tag(t) { printf("ctor %d/%d\n", id, tag); }
  void bump(int d) {
    id += d;
    printf("bump -> %d\n", id);
  }
  int get() const { return id; }
  ~Node() { printf("dtor %d\n", id); }
};

int main(int argc, char **argv) {
  box b;
  b.w = argc + 2;
  b.h = argc * 3;

  my_box mb;
  mb.d = b.w + b.h;
  int area = b.area();
  printf("w=%d h=%d area=%d d=%d\n", b.w, b.h, area, mb.d);

  // The box's fields feed the payload seeds through plain locals (a member
  // expression is not an admitted make_unique argument shape).
  int w = b.w;
  int h = b.h;

  // Function-scope box, with output after its last use so a "drop at end of
  // function" lowering would print the dtor line in the wrong place.
  std::unique_ptr<Node> p = std::make_unique<Node>(w, h);
  p->bump(argc);
  p->id = p->get() + area;
  printf("p.id=%d p.tag=%d get=%d\n", p->id, p->tag, p->get());

  // Loop-body boxes: one ctor/dtor pair per iteration, interleaved with the
  // body's own output.
  for (int i = 0; i < argc + 1; ++i) {
    std::unique_ptr<Node> q = std::make_unique<Node>(i + w, i);
    q->bump(i * 2 + 1);
    printf("iter %d -> %d\n", i, q->get());
  }

  printf("after loop w=%d\n", b.w);
  return 0;
}
