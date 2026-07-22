// REQUIRES: cargo
// Stage 5 of the owner-struct self-reference extension (design.md FR-30
// follow-on), differential end-to-end counterpart of the Stage 5
// import-only test (test/Import/C/array-self-ref-member-cross-param.c).
// Runs the CROSS-PARAMETER equality shape (B5) at actual runtime: `unite`
// independently traces two DIFFERENT parameters through `find`, compares
// the two results for equality, and — when unequal — repoints one root's
// `self` field at the other AND increments the other's plain `rank`
// field. This is the actual `union-find.c` `uf_union` shape (minus rank
// comparisons/union-by-rank, which are ordinary plain-field logic already
// covered elsewhere and add nothing to this stage's B5 de-risking).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: grep "enum_def\|struct.*_Bases\|Bases" %t.crate/src/main.rs
// RUN: grep "match" %t.crate/src/main.rs
// RUN: not grep "unsafe" %t.crate/src/main.rs
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/array_self_ref_member_cross_param > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct node { struct node *self; int rank; };

/* Owner method binding one array element's `self` field to another
   element (or itself): `target` is a second pointer parameter of this
   same method, already usable by Stage 2's original rule. */
void link_node(struct node *p, struct node *target) {
  p->self = target;
}

/* Owner method setting the plain (non-pointer) `rank` field. */
void set_rank(struct node *p, int rank) {
  p->rank = rank;
}

/* `find`: unchanged path-compressing walk from Stage 4. */
struct node *find(struct node *x) {
  struct node *parent;
  while (x->self != x) {
    parent = x->self;
    x->self = parent->self;
    x = parent;
  }
  return x;
}

/* `unite`: the Stage 5 shape. `a` and `b` are two INDEPENDENT pointer
   parameters, each traced through its own `find` call to a local (`ra`,
   `rb`). Comparing `ra == rb` is B5 — two DIFFERENT parameter-rooted
   bases that must nonetheless be recognized as cursors into the SAME
   promoted array class. When unequal, `ra`'s `self` field (the enum
   field) is repointed at `rb`, and `rb`'s `rank` field (a plain sibling
   field) is incremented, in that same cross-parameter-equality
   context. */
void unite(struct node *a, struct node *b) {
  struct node *ra = find(a);
  struct node *rb = find(b);
  if (ra == rb)
    return;
  ra->self = rb;
  rb->rank = rb->rank + 1;
}

int main(void) {
  struct node arr[4];

  set_rank(&arr[0], 100);
  set_rank(&arr[1], 101);
  set_rank(&arr[2], 102);
  set_rank(&arr[3], 103);

  /* Two independent 2-element chains inside ONE promoted class: arr[0] is
     its own root with arr[1] chained to it; arr[2] is its own root with
     arr[3] chained to it. */
  link_node(&arr[0], &arr[0]);
  link_node(&arr[1], &arr[0]);
  link_node(&arr[2], &arr[2]);
  link_node(&arr[3], &arr[2]);

  /* Before uniting: two disjoint groups. */
  int i;
  for (i = 0; i < 4; i++) {
    struct node *r = find(&arr[i]);
    printf("before i=%d root_rank=%d\n", i, r->rank);
  }

  /* Unite via two DIFFERENT parameters independently traced back through
     `find` — the B5 shape. */
  unite(&arr[1], &arr[3]);

  /* A second, redundant call: `ra == rb` now holds (same group), so
     `unite` must take the early-return path and leave every rank
     untouched — exercises the TRUE arm of the B5 comparison, not just
     the FALSE arm the first call exercised. */
  unite(&arr[0], &arr[2]);

  for (i = 0; i < 4; i++) {
    struct node *r = find(&arr[i]);
    printf("after i=%d root_rank=%d\n", i, r->rank);
  }

  return 0;
}
