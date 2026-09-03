// REQUIRES: cargo
// W2.21: std::unique_ptr -> Box<T> and std::make_unique -> Box::new, end
// to end. THE oracle for the wave: the emitted crate's stdout is diffed
// byte for byte against a `clang++ -std=c++17` build of the identical
// source. `cargo build` succeeding proves nothing here — every hazard
// below is invisible to a compile-clean crate.
//
// Bare Box, not Option<Box>: BOTH images were measured byte-identical
// during the W2.21 spike, so the choice is cost rather than correctness,
// and Option<Box<T>> would force an unwrap at every dereference an
// always-initialized program never needed (FR-99's precedent: "the local
// stays the bare struct"). What that costs is that every nullable
// spelling must reject; those pins live in
// test/Import/Cpp/stl-unique-ptr-invalid.cpp.
//
// What each line is here to catch:
//  - DROP TIMING, the whole reason a printf destructor is in this file. A
//    Box drops its payload, so the C++ destructor and the Rust `Drop`
//    must fire at the same program point. The boxes sit in a LOOP body
//    (one ctor/dtor pair per iteration, interleaved with the body's own
//    output), in a BRANCH body, and at function scope with output after
//    the last use — three places where a "drop at end of function"
//    lowering, or a dropped dead binding, prints the dtor lines in the
//    wrong order. `hold`'s two boxes additionally pin REVERSE declaration
//    order, which C++ and Rust agree on and an insertion-ordered drop
//    would not.
//  - THE ACCESS SPLIT. `p->m()` / `(*p).m()` lower to a method call
//    DIRECTLY on the Box place (Rust auto-deref); `p->field` and `*n` go
//    through a Deref/DerefMut borrow. Routing the method receiver through
//    the borrow too builds the receiver's `&mut` BEFORE the argument
//    values, which is rustc E0502 for `p->bump(p->get())` below.
//  - READ BORROWS STAY SHARED: two field reads of one box live at once in
//    a single printf is rustc E0499 if the borrow is taken mutably.
//  - C++17 EVALUATION ORDER (P0145R3): `p->id = p->get() + 1` sequences
//    the right-hand side first, and the payload place holds a mutable
//    borrow of the Box for as long as it lives, so an LHS-place-first
//    lowering is rustc E0502.
//  - THE TWO-STEP CONSTRUCTION `Box::new(T::default())` + ctor-as-method
//    must print the ctor line exactly once and must not drop the
//    default-constructed payload (Box::new MOVES it).
//  - A CLASS-TEMPLATE payload (W2.16) must box the MONOMORPHIZED struct
//    name, and a POD payload with no constructor at all must
//    value-initialize to zero exactly as `new T()` does.
//  - FR-188 REFERENCE ARGUMENTS out of the payload. `refargs` is the
//    RUNTIME half of the over-rejection guard in
//    test/Import/Cpp/stl-unique-ptr-ref-argument.cpp: a `const T &`
//    parameter is faithfully served by the SHARED payload borrow, and a
//    by-value parameter by a load, so both must keep importing AND keep
//    printing what clang++ prints. The MUTABLE sibling (`h(*p)`) is a
//    located rejection instead, because the shared borrow cannot satisfy
//    it -- it emitted rustc E0596, or, for `(*p).field`, a crate that
//    built clean and threw the callee's write away. A FileCheck of the IR
//    cannot tell those two apart from working code; this diff can, which
//    is why the positive legs live here and not only in the golden.
//
// Every value derives from argc, so no constant folding can pre-compute
// the answers and hide a miscompile behind a compile-clean crate. argv is
// a hard rejection here — argc only.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_unique_ptr > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <memory>

struct Node {
  int id;
  int tag;
  Node(int i, int t) : id(i), tag(t) { printf("ctor %d/%d\n", id, tag); }
  void bump(int d) { id += d; printf("bump -> %d\n", id); }
  int get() const { return id; }
  ~Node() { printf("dtor %d\n", id); }
};

template <typename T> struct Cell {
  T v;
  Cell(T x) : v(x) { printf("cell ctor %d\n", (int)v); }
  T get() const { return v; }
  void set(T x) { v = x; }
  ~Cell() { printf("cell dtor %d\n", (int)v); }
};

struct Pod {
  int a;
  int b;
};

// FR-188 callees. `constsum`/`constfield` take SHARED references, which
// the payload's shared borrow serves directly; `byvalsum` takes a
// payload by value, which is a load and no borrow at all (a POD payload,
// so the copy brings no destructor timing of its own into the diff).
// Their mutable sibling `void h(Node &)` is a located rejection and so
// cannot appear in a byte-diff test -- its pin is in
// test/Import/Cpp/stl-unique-ptr-invalid.cpp.
static int constsum(const Node &n) { return n.id * 2 + n.tag; }
static int constfield(const int &x) { return x + 3; }
static int byvalsum(Pod q) { return q.a + q.b * 5; }

// Scalar payloads: read, compound write, whole write, and the
// value-initializing zero-argument make_unique.
static int scalars(int seed) {
  auto n = std::make_unique<int>(seed * 7);
  printf("n=%d\n", *n);
  *n += 2;
  *n = *n * 2;
  printf("n=%d\n", *n);
  auto z = std::make_unique<int>();
  printf("z=%d\n", *z);
  std::unique_ptr<int> e = std::make_unique<int>(seed + 1);
  printf("e=%d\n", *e);
  return *n + *z + *e;
}

// A POD payload with no user constructor: `new Pod()` value-initializes,
// and the field places are ordinary member projections through the
// Deref/DerefMut borrow.
static int pod(int seed) {
  auto p = std::make_unique<Pod>();
  printf("pod0=%d,%d\n", p->a, p->b);
  p->a = seed;
  p->b = p->a + 4;
  printf("pod1=%d,%d\n", p->a, p->b);
  return p->a + p->b;
}

// A class-template instantiation payload (W2.16): the Box must name the
// MONOMORPHIZED struct, and the destructor must still fire on scope exit.
static int templated(int seed) {
  auto c = std::make_unique<Cell<int>>(seed + 40);
  printf("cell=%d\n", c->get());
  c->set(c->get() + 1);
  printf("cell=%d\n", (*c).get());
  printf("before cell scope end\n");
  return c->get();
}

// Drop timing from a loop body and a branch body, plus the E0502 shape
// `p->bump(p->get())` and the P0145R3 shape `p->id = p->get() + 1`.
static int timing(int seed) {
  auto p = std::make_unique<Node>(seed + 10, seed);
  printf("field=%d tag=%d\n", p->id, p->tag);
  p->bump(p->get());
  printf("method=%d\n", p->get());
  (*p).bump(5);
  printf("deref=%d\n", (*p).get());
  p->id = p->get() + 1;
  p->tag += 3;
  p->tag++;
  printf("assigned=%d tag=%d\n", p->id, p->tag);
  for (int i = 0; i < 3; i++) {
    auto q = std::make_unique<Node>(100 + i * seed, i);
    q->bump(i);
    printf("loop %d -> %d\n", i, q->get());
  }
  printf("after loop\n");
  if (seed > 0) {
    auto b = std::make_unique<Node>(200 + seed, 9);
    printf("branch=%d\n", b->get());
  }
  printf("after branch\n");
  printf("before fn end\n");
  return p->get();
}

// Two boxes in one scope: C++ and Rust both destroy in REVERSE
// declaration order, so `dtor 302` must print before `dtor 301`.
static int hold(int seed) {
  auto first = std::make_unique<Node>(300 + seed, 1);
  auto second = std::make_unique<Node>(301 + seed, 2);
  printf("held %d %d\n", first->get(), second->get());
  printf("before hold end\n");
  return first->get() + second->get();
}

// FR-188: the shapes that must SURVIVE the mutable-reference-argument
// rejection. Every one of these reads the payload through the shared
// `Deref::deref` borrow, which is exactly what `const T &` and a
// by-value parameter mean, so the values printed here must match clang++
// exactly. `seed` reaches every one of them, so nothing folds.
//
// The `(*p).field` READ spelling is deliberately absent: it is a
// SEPARATE, pre-existing defect that FR-188 diagnosed but does not fix.
// Because no payload-place predicate recognized the non-arrow member
// form, `(*p).tag` loads the WHOLE payload out of the shared borrow into
// a staged local before projecting, which is rustc E0507 the moment the
// payload is not `Copy` (a user destructor is enough). Adding it here
// would pin a shape that does not build. Its mutable-argument sibling IS
// fixed here, by rejection, in stl-unique-ptr-invalid.cpp's
// free-ref-star-field leg -- that one built clean and silently discarded
// the callee's write, which is the worse failure and the one that could
// not be left alone.
static int refargs(int seed) {
  auto p = std::make_unique<Node>(seed + 60, seed + 61);
  printf("refargs const=%d\n", constsum(*p));
  printf("refargs field=%d\n", constfield(p->id));
  printf("refargs tagfield=%d\n", constfield(p->tag));
  auto q = std::make_unique<Pod>();
  q->a = seed + 62;
  q->b = seed + 63;
  printf("refargs byval=%d\n", byvalsum(*q));
  auto s = std::make_unique<int>(seed + 70);
  printf("refargs scalar=%d\n", constfield(*s));
  printf("refargs untouched=%d,%d,%d,%d\n", p->id, p->tag, q->a, q->b);
  printf("before refargs end\n");
  return constsum(*p) + byvalsum(*q) + *s;
}

int main(int argc, char **) {
  printf("scalars=%d\n", scalars(argc));
  printf("pod=%d\n", pod(argc));
  printf("templated=%d\n", templated(argc));
  printf("timing=%d\n", timing(argc));
  printf("hold=%d\n", hold(argc));
  printf("refargs=%d\n", refargs(argc));
  printf("main end\n");
  return 0;
}
