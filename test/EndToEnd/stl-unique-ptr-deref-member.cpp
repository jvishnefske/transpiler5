// REQUIRES: cargo
// FR-189: `(*p).field` ON A std::unique_ptr PAYLOAD IS THE SAME OPERATION
// AS `p->field`, AND THIS DIFF IS THE ONLY ORACLE THAT CAN SAY SO.
//
// Before FR-189 the non-arrow member spelling was not recognized as a
// payload place at all, and the two halves failed in the two worst ways
// this repo knows:
//
//  (a) WRITE -- A SILENT MISCOMPILE. `(*p).id = v` took the SHARED
//      `Deref::deref` borrow, loaded the WHOLE payload into a fresh
//      staged local, stored into the COPY, and discarded it. Measured on
//      exactly the shape below: clang++ printed `id=6 tag=2`, the
//      emitted crate printed `id=0 tag=0`. `cargo build` was CLEAN.
//      A FileCheck of the IR cannot tell that apart from working code;
//      a byte-diff can, which is why the load-bearing pin is here and
//      not in a golden.
//  (b) READ -- an UNBUILDABLE crate. The same whole-payload load out of
//      a shared borrow is rustc E0507 the moment the payload is not
//      `Copy`, and a user destructor is enough. `Node` and `Outer` below
//      both have one DELIBERATELY: without it the read half silently
//      passes on a `Copy` payload and this file would stop pinning (b).
//
// The fix is WIDENING, not rejection: `p->field` already worked, so the
// two spellings are one operation and must converge on one path. That
// claim is what `agree=` pins -- every function below is computed twice,
// once through `->` and once through `(*...)`, and the two results are
// compared IN THE PROGRAM as well as byte-diffed against clang++. A
// regression that breaks only the non-arrow spelling shows up twice.
//
// What each leg is here to catch:
//  - WHOLE-FIELD STORE `(*p).id = v`, the FR-189 headline: `id=6 tag=2`.
//  - COMPOUND `(*p).tag += 3` and INCREMENT `(*p).tag++`: both go
//    through the same write-context predicate and both silently wrote to
//    a discarded copy before.
//  - NON-ARROW READS through the shared borrow: a bare value read, a
//    `const int &` argument (`constfield((*p).tag)`) and a `const Node &`
//    argument (`constsum(*p)`). These must project the FIELD out of the
//    borrow, never move the payload.
//  - NESTED `(*q).a.x` and SUBSCRIPT `(*q).arr[i]` chains, which the
//    FR-188 walker `matchStlBoxPayloadPlaceBase` admits. Their ARROW
//    siblings `q->a.x = v` / `q->arr[i] = v` were ALSO broken before
//    FR-189 (rustc E0594, `p` never declared mutable, whenever no other
//    write made the Box mutable) -- so `arrow_nested` is a pin in its
//    own right, not just a control.
//  - DROP TIMING is unchanged by the widening: every payload here has a
//    printf destructor and the dtor lines must stay in the same places.
//
// Every value derives from argc, so no constant folding can pre-compute
// the answers and hide a miscompile behind a compile-clean crate. argv is
// a hard rejection here -- argc only.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_unique_ptr_deref_member > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <memory>

struct Inner {
  int x;
  int y;
};

// The destructor is load-bearing, not decoration: it makes `Node`
// non-`Copy`, which is the only condition under which the non-arrow READ
// half was rustc E0507.
struct Node {
  int id;
  int tag;
  Node(int i, int t) : id(i), tag(t) { printf("ctor %d/%d\n", id, tag); }
  int get() const { return id; }
  ~Node() { printf("dtor %d\n", id); }
};

// Likewise non-`Copy`, and with a nested struct field and an array field
// so the member/subscript projection chains have somewhere to go.
struct Outer {
  Inner a;
  int arr[4];
  int z;
  ~Outer() { printf("outer dtor %d\n", z); }
};

// Shared-reference callees. `const Node &` is served by the payload's
// shared borrow directly; `const int &` by a FIELD projection out of that
// same borrow -- which is precisely what the broken read half did not do.
static int constsum(const Node &n) { return n.id * 2 + n.tag; }
static int constfield(const int &x) { return x + 3; }

// The WORKING spelling, computed first so the non-arrow leg below has a
// value to agree with.
static int arrow_writes(int seed) {
  auto p = std::make_unique<Node>(seed, seed);
  p->id = seed + 5;
  p->tag = seed + 1;
  printf("id=%d tag=%d\n", p->id, p->tag);
  p->tag += 3;
  p->tag++;
  p->id -= 2;
  printf("id=%d tag=%d\n", p->id, p->tag);
  printf("const=%d field=%d\n", constsum(*p), constfield(p->tag));
  printf("before arrow end\n");
  return p->get();
}

// FR-189 half (a) and half (b), the identical program spelled `(*p).`.
// The first two stores are the exact measured repro: native `id=6 tag=2`,
// pre-fix crate `id=0 tag=0`.
static int star_writes(int seed) {
  auto p = std::make_unique<Node>(seed, seed);
  (*p).id = seed + 5;
  (*p).tag = seed + 1;
  printf("id=%d tag=%d\n", (*p).id, (*p).tag);
  (*p).tag += 3;
  (*p).tag++;
  (*p).id -= 2;
  printf("id=%d tag=%d\n", (*p).id, (*p).tag);
  printf("const=%d field=%d\n", constsum(*p), constfield((*p).tag));
  printf("before arrow end\n");
  return (*p).get();
}

// The nested and subscript chains through `->`. Not merely a control:
// `q->a.x = v` and `q->arr[i] = v` were themselves unbuildable before
// FR-189 unless some other statement had already forced the Box binding
// to be mutable.
static int arrow_nested(int seed) {
  auto q = std::make_unique<Outer>();
  q->a.x = seed + 7;
  q->a.y = q->a.x * 2;
  q->arr[1] = seed + 9;
  q->arr[seed] = q->arr[1] + 1;
  q->z = seed;
  printf("nested %d %d %d %d %d\n", q->a.x, q->a.y, q->arr[0], q->arr[1], q->z);
  printf("nestedref %d %d\n", constfield(q->a.y), constfield(q->arr[1]));
  printf("before nested end\n");
  return q->a.x + q->a.y + q->arr[1] + q->z;
}

// The same chains spelled `(*q).`, which is what
// `matchStlBoxPayloadPlaceBase` walks.
static int star_nested(int seed) {
  auto q = std::make_unique<Outer>();
  (*q).a.x = seed + 7;
  (*q).a.y = (*q).a.x * 2;
  (*q).arr[1] = seed + 9;
  (*q).arr[seed] = (*q).arr[1] + 1;
  (*q).z = seed;
  printf("nested %d %d %d %d %d\n", (*q).a.x, (*q).a.y, (*q).arr[0],
         (*q).arr[1], (*q).z);
  printf("nestedref %d %d\n", constfield((*q).a.y), constfield((*q).arr[1]));
  printf("before nested end\n");
  return (*q).a.x + (*q).a.y + (*q).arr[1] + (*q).z;
}

int main(int argc, char **) {
  int arrow = arrow_writes(argc);
  int star = star_writes(argc);
  printf("arrow=%d star=%d agree=%d\n", arrow, star, arrow == star);
  int arrowN = arrow_nested(argc);
  int starN = star_nested(argc);
  printf("nested=%d starnested=%d agree=%d\n", arrowN, starN, arrowN == starN);
  printf("main end\n");
  return 0;
}
