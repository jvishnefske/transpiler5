// REQUIRES: cargo
// Stage 4 of the owner-struct self-reference extension (design.md FR-30
// follow-on), differential end-to-end counterpart of the Stage 4 import-only
// test (test/Import/C/array-self-ref-member-chain.c). Runs the CHAINED
// self-reference (path compression) mechanism at actual runtime: `find`
// walks and compresses a chain built by `link_node`, exercising all three
// Stage 4 pieces the master plan calls B3 (`parent = x->self;`, a local
// bound from a field read), B4 (`x->self = parent->self;`, a field write
// whose right-hand side is itself a field read), and the loop-comparison
// shape (`while (x->self != x)`). This is the actual `union-find.c` `uf_find`
// shape (minus the `union-find.c`-specific cross-parameter equality of two
// DIFFERENT `find` results, which is Stage 5's B5 scope, not this test's).
// Every node's `rank` field is set to a distinctive value (its own original
// index) purely so the differential output can identify, after compression,
// WHICH physical array element each `find` call landed on, without needing
// pointer arithmetic C11 does not define across unrelated calls.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: grep "enum_def\|struct.*_Bases\|Bases" %t.crate/src/main.rs
// RUN: grep "match" %t.crate/src/main.rs
// RUN: not grep "unsafe" %t.crate/src/main.rs
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/array_self_ref_member_chain > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct node { struct node *self; int rank; };

/* Owner method binding one array element's `self` field to another element
   (or itself): `target` is a second pointer parameter of this same method,
   already usable by Stage 2's original rule — no Stage 4 change needed. */
void link_node(struct node *p, struct node *target) {
  p->self = target;
}

/* Owner method setting the plain (non-pointer) `rank` field; ordinary
   struct-member write, unaffected by the array-member-pointer machinery. */
void set_rank(struct node *p, int rank) {
  p->rank = rank;
}

/* `find`: the actual path-compression shape `union-find.c`'s `uf_find`
   needs. `parent = x->self;` is B3; `x->self = parent->self;` is B4;
   `while (x->self != x)` is the loop-comparison shape. Every call mutates
   the chain (path compression), so later calls in the same run see an
   already-partially-compressed structure — deterministic and identical
   between the native and transpiled builds. */
struct node *find(struct node *x) {
  struct node *parent;
  while (x->self != x) {
    parent = x->self;
    x->self = parent->self;
    x = parent;
  }
  return x;
}

int main(void) {
  struct node arr[4];

  set_rank(&arr[0], 100);
  set_rank(&arr[1], 101);
  set_rank(&arr[2], 102);
  set_rank(&arr[3], 103);

  /* A chain: arr[0] is its own root; arr[1]->arr[0]; arr[2]->arr[1];
     arr[3]->arr[2]. */
  link_node(&arr[0], &arr[0]);
  link_node(&arr[1], &arr[0]);
  link_node(&arr[2], &arr[1]);
  link_node(&arr[3], &arr[2]);

  int i;
  for (i = 0; i < 4; i++) {
    struct node *r = find(&arr[i]);
    printf("i=%d root_rank=%d\n", i, r->rank);
  }

  return 0;
}
