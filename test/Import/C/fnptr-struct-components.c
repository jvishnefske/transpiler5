// FR-102: STRUCT-POINTER components in function-pointer types. Before
// this, a component whose pointee was not arithmetic funneled into
// `mapType`'s pointer residual and rejected the WHOLE record, which made
// "pointer type outside a parameter position" the #1 measured blocker in
// the external corpus (110 items plus cascades). This pins the new
// contract: a component whose pointee is a COMPLETE record maps through
// exactly the `mapParamType` ScalarRef branch an ordinary struct-pointer
// parameter takes, so `int (*)(struct payload *, int)` becomes
// `!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"payload">>, i32)
// -> i32>` and the field, the `Some(f)` binding and the call through the
// field all import. Self-reference falls out of the same admission rather
// than needing its own design: `importRecord`'s `importedRecords` guard
// fires before the field walk and the struct type is built BY NAME, so a
// component naming the record under construction terminates. Rust needs
// no lifetime annotation — `fn(&mut Node, i32) -> i32` is higher-ranked
// and elides — so the emitter is unchanged.
//
// The mapping deliberately REUSES the ordinary parameter mapping rather
// than inventing a component-only one, which is what keeps binding
// equality (`resolveFunctionPointerDecl`) working; the consequence is
// that a record pointee the parameter mapper spells differently is spelled
// the same way here, so the byte-region and FAM flavors are pinned below
// so they cannot drift silently.
//
// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-import-c %s | FileCheck --check-prefix=DEF %s

struct payload { int lo; int hi; };

// An UNRELATED complete struct: this is the shape the spike proved is the
// real blocker (self-reference was never the issue — a component naming a
// complete unrelated struct rejected identically before FR-102).
struct ops {
  int (*apply)(struct payload *p, int n);
  int tag;
};
// DEF: emitrust.struct_def @ops ["apply", "tag"] [!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"payload">>, i32) -> i32>, i32]

// SELF-REFERENTIAL component: the record names its own type while it is
// mid-import. This is the design prediction the FR-102 spike confirmed.
struct node {
  int val;
  int (*visit)(struct node *n, int depth);
};
// DEF: emitrust.struct_def @node ["val", "visit"] [i32, !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"node">>, i32) -> i32>]

// A `const struct S *` pointee: FR-80's shared-const-record rule is
// signature-builder gated and does not engage here, so the component is
// the SAME `mut_ref` the ordinary parameter mapping produces — which is
// exactly why the `reader` binding below unifies.
struct cfg { int x; };
struct reader_ops {
  void (*read)(const struct cfg *p);
};
// DEF: emitrust.struct_def @reader_ops ["read"] [!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"cfg">>)>]

// A UNION pointee rides the one-slot union model.
union tagged { int i; float f; };
struct union_ops {
  int (*take)(union tagged *u);
};
// DEF: emitrust.struct_def @union_ops ["take"] [!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"tagged">>) -> i32>]

// A BYTE-REGION record pointee (all-u8 fields) is spelled by
// `mapParamType`'s `isByteRegionAggregate` arm as a byte slice, NOT as a
// `&mut Record` — surprising, but consistent with the ordinary parameter
// mapping, which is what binding equality depends on. Pinned so it cannot
// drift.
struct bytes { unsigned char a; unsigned char b; };
struct byte_ops {
  int (*hash)(struct bytes *b);
};
// DEF: emitrust.struct_def @byte_ops ["hash"] [!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.slice<ui8>>) -> i32>]

int visit_double(struct node *n, int depth) {
  n->val = n->val * 2 + depth;
  return n->val;
}
// CHECK-LABEL: func.func @visit_double
// CHECK-SAME: !emitrust.mut_ref<!emitrust.struct<"node">>

int add_bounds(struct payload *p, int n) {
  p->lo += n;
  p->hi -= n;
  return p->lo + p->hi;
}
// CHECK-LABEL: func.func @add_bounds
// CHECK-SAME: !emitrust.mut_ref<!emitrust.struct<"payload">>

// A DEFINED `const struct cfg *` callee: the address-taken forcing leaves
// it ScalarRef, so its own signature equals the component type exactly.
int sink;
void reader(const struct cfg *p) { sink = p->x; }
// CHECK-LABEL: func.func @reader
// CHECK-SAME: !emitrust.mut_ref<!emitrust.struct<"cfg">>

int main(void) {
  struct ops s;
  // Binding a real function to a struct-pointer-component field is the
  // `Some(f)` constant of the field's own fn_ptr type; the FR-29 equality
  // check at `resolveFunctionPointerDecl` is what proves the two
  // signatures unified.
  s.apply = add_bounds;
  s.tag = 4;
  // CHECK-LABEL: func.func @c_main
  // CHECK: emitrust.constant <#emitrust.opaque<"Some(add_bounds)">> : !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"payload">>, i32) -> i32>
  struct payload q;
  q.lo = 1;
  q.hi = 2;
  // The indirect call through the member passes a mutable borrow of the
  // local, exactly like a direct call to a struct-pointer callee.
  int r = s.apply(&q, 1) + s.tag;
  // CHECK: emitrust.addr_of mut
  // CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}, %{{.*}}) : (!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"payload">>, i32) -> i32>, !emitrust.mut_ref<!emitrust.struct<"payload">>, i32) -> i32

  // Self-alias: the receiver of the indirect call is the very object that
  // holds the fn pointer. The fn pointer is a Copy value read out before
  // the `&mut` is taken, so Rust's borrow checker accepts it.
  struct node nd;
  nd.val = 3;
  nd.visit = visit_double;
  // CHECK: emitrust.constant <#emitrust.opaque<"Some(visit_double)">> : !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"node">>, i32) -> i32>
  int v = nd.visit(&nd, 2);
  // CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}, %{{.*}}) : (!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"node">>, i32) -> i32>, !emitrust.mut_ref<!emitrust.struct<"node">>, i32) -> i32

  // A LOCAL of the same fn-ptr type, and a const-pointee binding.
  struct reader_ops ro;
  ro.read = reader;
  // CHECK: emitrust.constant <#emitrust.opaque<"Some(reader)">> : !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"cfg">>)>
  int (*held)(struct node *, int) = visit_double;
  // CHECK: emitrust.variable named "held" : !emitrust.lvalue<!emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"node">>, i32) -> i32>>
  int h = held(&nd, 1);
  // CHECK: emitrust.call_indirect
  return (r + v + h + sink) & 1;
}
