// REQUIRES: cargo
// Stage 3 of the owner-struct self-reference extension (design.md FR-37
// follow-on), differential end-to-end counterpart of the Stage 2
// import-only test (test/Import/C/array-self-ref-member.c). Same shape:
// `struct node`'s `self` field always points at an element of the SAME
// promoted owner array (`arr` in `main`, promoted by `planOwners` because
// it crosses a function boundary through `link_node`/`check_node`'s
// pointer parameters), so `planArrayMemberPointers` proves the field a
// closed-set self-reference and synthesizes an `emitrust.enum_def`
// (one variant per array index) for it instead of a plain i64 cursor.
// This test runs that mechanism at actual runtime: `link_node` WRITES the
// enum-typed field (lowering to a genuine `match`) and also increments
// the plain `x` field alongside it (proving the enum field and an
// ordinary field coexist correctly in the same struct/method); the loop
// in `main` READS the field back through `check_node`'s `p->self == p`
// equality (decoding the enum with no branching) and prints both the
// equality result and the plain field, giving the differential harness
// real per-element output to diff between native C and the transpiled
// Rust. No path compression, no multi-step chains, no cross-parameter
// equality — those are later stages.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: grep "enum_def\|struct.*_Bases\|Bases" %t.crate/src/main.rs
// RUN: grep "match" %t.crate/src/main.rs
// RUN: not grep "unsafe" %t.crate/src/main.rs
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/array_self_ref_member > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

struct node { struct node *self; int x; };

/* Owner method: the arrow WRITE of `self` roots in the same class as `p`
   itself, so Pass A proves the field usable and lowers this write to a
   genuine Rust `match`. The plain `x` field is incremented alongside it
   to exercise a plain-field-alongside-enum-field case. */
void link_node(struct node *p) {
  p->self = p;
  p->x = p->x + 1;
}

/* Owner method: the arrow READ of `self` also roots in the same class,
   validating the read side (branchless enum decode) independently of the
   write, at runtime this time. */
int check_node(struct node *p) {
  return p->self == p;
}

int main(void) {
  struct node arr[3];
  arr[0].x = 10;
  arr[1].x = 20;
  arr[2].x = 30;

  link_node(&arr[0]);
  link_node(&arr[1]);
  link_node(&arr[2]);

  int i;
  for (i = 0; i < 3; i++) {
    printf("i=%d same=%d x=%d\n", i, check_node(&arr[i]), arr[i].x);
  }

  return 0;
}
