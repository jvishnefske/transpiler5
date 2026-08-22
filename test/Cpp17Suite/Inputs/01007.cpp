#include <memory>

extern "C" int printf(const char *, ...);

// W2.21: the first corpus entry over std::unique_ptr.
//
// std::unique_ptr<T> -> Box<T>, std::make_unique<T>(args) -> Box::new.
// Bare Box, not Option<Box>: both images were measured byte-identical
// against clang++ -std=c++17, so the choice is cost rather than
// correctness, and Option<Box<T>> would force an unwrap at every
// dereference an always-initialized program never needed. The
// always-initialized subset is exactly what this entry exercises; every
// nullable spelling is a located rejection (see
// test/Import/Cpp/stl-unique-ptr-invalid.cpp).
//
// The hazards this entry exists to catch, none of which a compile-clean
// `cargo build` can see:
//  - DROP TIMING. A Box drops its payload, so the C++ destructor and the
//    Rust `Drop` must fire at the same program point. The printf
//    destructor below makes that observable, and the boxes are placed in
//    a LOOP body (one ctor/dtor pair per iteration, interleaved with the
//    body's own output) and in a BRANCH body (dtor before "after
//    branch"), which are the two scopes where a naive "drop at end of
//    function" lowering would print the dtor lines in the wrong place.
//  - THE ACCESS SPLIT. `p->m()` and `(*p).m()` lower to a method call
//    DIRECTLY on the Box place (Rust auto-deref), while `p->field` and
//    `*n` go through a Deref/DerefMut borrow. Routing the method receiver
//    through the borrow too builds the receiver's `&mut` BEFORE the
//    argument values, which is rustc E0502 for `p->bump(p->get())`.
//  - READ BORROWS MUST STAY SHARED. Two field reads of one box in a
//    single printf (`p->id, p->tag`) are two live borrows; taking them
//    mutably is rustc E0499.
//  - C++17 EVALUATION ORDER (P0145R3): `p->id = p->get() + 1` sequences
//    the right-hand side first, and the payload place holds a MUTABLE
//    borrow of the Box for as long as it lives, so an LHS-place-first
//    lowering is rustc E0502.
//  - THE TWO-STEP CONSTRUCTION. `Box::new(Node::default())` followed by
//    the constructor as a `&mut self` method must print the ctor line
//    exactly once, at the make_unique site.
//
// Every value is derived from argc so no constant folding can pre-compute
// the answers; argv itself is never touched.

struct Node {
  int id;
  int tag;
  Node(int i, int t) : id(i), tag(t) { printf("ctor %d/%d\n", id, tag); }
  void bump(int d) { id += d; printf("bump -> %d\n", id); }
  int get() const { return id; }
  ~Node() { printf("dtor %d\n", id); }
};

static int owned(int seed) {
  auto n = std::make_unique<int>(seed * 3);
  *n += 1;
  printf("scalar=%d\n", *n);
  auto p = std::make_unique<Node>(seed + 10, seed);
  printf("field=%d tag=%d\n", p->id, p->tag);
  p->bump(2);
  printf("method=%d\n", p->get());
  (*p).bump(5);
  printf("deref=%d\n", (*p).get());
  p->id = p->get() + 1;
  printf("assigned=%d\n", p->id);
  for (int i = 0; i < 2; i++) {
    auto q = std::make_unique<Node>(100 + i + seed, i);
    q->bump(i);
    printf("loop %d -> %d\n", i, q->get());
  }
  printf("after loop\n");
  if (seed > 0) {
    auto b = std::make_unique<Node>(200 + seed, 9);
    printf("branch=%d\n", b->get());
  }
  printf("after branch\n");
  int total = p->get() + *n;
  printf("before fn end\n");
  return total;
}

int main(int argc, char **) {
  printf("total=%d\n", owned(argc));
  printf("main end\n");
  return 0;
}
