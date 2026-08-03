// RUN: emitrust-import-c %s | FileCheck %s

// FR-48: C++ lvalue reference PARAMETERS, the first reference position the
// importer accepts. Every earlier wave rejected `T&` outright with
// "reference types are not yet supported" (see cpp-basics-invalid.cpp's
// REFPARAM case, which this wave retires), because a reference has no C
// analogue the pointer machinery could reuse verbatim: a pointer parameter
// is an ordinary SSA value the body may reseat, whereas a reference is a
// borrow that names a caller-owned place for the whole call and can never
// be rebound.
//
// The PIN, and the reason this shape is not new ground: FR-48 deliberately
// spells a reference with the SAME two ops a pointer parameter's `*p`
// already emits, so nothing downstream of the importer had to learn a new
// vocabulary. A reference parameter differs from a pointer parameter in
// exactly two places, both fixed at signature time:
//
//   * the PARAMETER TYPE is a borrow, not a raw pointer —
//     `const T&` -> `!emitrust.ref<T>`   (shared borrow, `&T`)
//     `T&`       -> `!emitrust.mut_ref<T>` (unique borrow, `&mut T`)
//     with the mutability decided ONCE, by the CALLEE's declared constness,
//     never by what the body happens to do with it; and
//
//   * the CALL SITE borrows the argument's PLACE instead of reading it —
//     `emitrust.addr_of [mut] %place`, with `is_mut` mirroring the
//     parameter's mutability, so a `const T&` argument never demands a
//     unique borrow it does not need. This is the same op the implicit
//     `this` receiver already used (cpp-implicit-this.cpp), reused
//     unchanged for an explicit parameter.
//
// In the BODY there is no third rule: a use of the reference parameter
// derefs the borrow into a place and then reads or writes that place,
//
//   %p = emitrust.deref %arg     -> !emitrust.lvalue<T>
//   %v = emitrust.load %p        (a read)
//   emitrust.assign %p = %v      (a write)
//
// which is byte-for-byte what `*p` on an `int *` parameter emits. That
// equality is the whole point of the design and is what these checks
// defend: a future change that gives references their own body lowering
// shows up here as a diff rather than as a silent divergence between the
// two spellings.
//
// Also pinned: per-USE deref. `read_point` below touches `p` twice and
// materializes TWO independent `emitrust.deref`s rather than hoisting one.
// A hoisted place would outlive its use and turn a sequence of
// short-lived borrows into one long-lived one, which is precisely the
// shape Rust's borrow checker rejects; keeping the deref at the use site
// is what keeps the emitted Rust borrow-checkable, exactly as it is for
// the repeated receiver borrows in cpp-implicit-this.cpp.
//
// SCOPE NOTE. This wave accepts references only in the PARAMETER position.
// Reference returns, reference struct members, rvalue references,
// reference-to-pointer and reference-to-array parameters, and reference
// LOCALS all remain rejected, each with its own located diagnostic; that
// ledger lives in cpp-references-invalid.cpp.
//
// One further shape is rejected for SOUNDNESS rather than for want of
// implementation: a method call whose reference argument names the same
// object as the receiver (`a.m(a)`, `m(*this)`) borrows one object twice.
// That is accepted when both borrows are shared and rejected with
// "aliasing mutable reference argument and method receiver" when either is
// mutable, mirroring the same-base rejection the C pointer path already
// applies to `f(&a, &a)`.

struct Point {
  int x;
  int y;
};

// `const T&` on a scalar: a shared borrow, read through.
int read_scalar(const int &v) { return v + 1; }

// `T&` on a scalar: a unique borrow, WRITTEN through. The write is an
// `emitrust.assign` to the dereffed place -- there is no reference-specific
// store op.
void write_scalar(int &v) { v = 7; }

// `const T&` on a struct: shared borrow, two member READS, and therefore
// two independent derefs (the per-use rule above).
int read_point(const Point &p) { return p.x + p.y; }

// `T&` on a struct: unique borrow, member WRITE. The `emitrust.member`
// projection hangs off the dereffed place exactly as it would off a local
// struct variable's place.
void write_point(Point &p) { p.x = 3; }

// Reference parameters on class METHODS: the receiver borrow and the
// explicit reference parameter coexist in one signature, and the body
// derefs each independently. `add` mixes a `mut_ref` receiver (non-const
// method) with a `ref` parameter (`const int&`); `take` mixes a `mut_ref`
// receiver with a `mut_ref` parameter, proving the two mutabilities are
// decided separately -- the receiver's by the METHOD's constness, the
// parameter's by the PARAMETER's constness. Neither is called; see the
// SCOPE NOTE above.
class Accum {
public:
  void add(const int &v) { total_ = total_ + v; }
  void take(Point &p) { p.y = total_; }
  int total() const { return total_; }

private:
  int total_;
};

// The call sites. Each argument is a plain local; the importer takes its
// place and borrows it, so nothing is copied and no temporary is staged.
int drive(void) {
  int n = 5;
  Point pt;
  pt.x = 1;
  pt.y = 2;
  int a = read_scalar(n);
  write_scalar(n);
  int b = read_point(pt);
  write_point(pt);
  Accum acc;
  // Method call sites: the receiver borrow and the argument borrow are both
  // live at the call, with independent mutability.
  acc.add(n);
  acc.take(pt);
  return a + b + acc.total();
}

// CHECK: emitrust.struct_def @Point ["x", "y"] [i32, i32]

// `const int&` -> a SHARED borrow in the signature, and a read that is
// deref-then-load, identical to `*p` on an `int *`.
// CHECK-LABEL: func.func @read_scalar
// CHECK-SAME: (%[[RSV:.*]]: !emitrust.ref<i32>) -> i32
// CHECK: %[[RSP:.*]] = emitrust.deref %[[RSV]] : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: %[[RSL:.*]] = emitrust.load %[[RSP]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: arith.addi %[[RSL]]

// `int&` -> a UNIQUE borrow, and a write that is deref-then-assign. Note
// there is NO load: binding a reference does not read the referent, so a
// pure write through one emits no read of the caller's value.
// CHECK-LABEL: func.func @write_scalar
// CHECK-SAME: (%[[WSV:.*]]: !emitrust.mut_ref<i32>)
// CHECK: %[[WSP:.*]] = emitrust.deref %[[WSV]] : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: %[[WSC:.*]] = arith.constant 7 : i32
// CHECK-NEXT: emitrust.assign %[[WSP]] = %[[WSC]] : !emitrust.lvalue<i32>

// `const Point&` -> a shared borrow of the STRUCT, with member projection
// off the dereffed place. Two uses of `p`, two derefs: the per-use rule.
// CHECK-LABEL: func.func @read_point
// CHECK-SAME: (%[[RPV:.*]]: !emitrust.ref<!emitrust.struct<"Point">>) -> i32
// CHECK: %[[RPP0:.*]] = emitrust.deref %[[RPV]] : (!emitrust.ref<!emitrust.struct<"Point">>) -> !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK-NEXT: %[[RPX:.*]] = emitrust.member %[[RPP0]]["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: %[[RPXV:.*]] = emitrust.load %[[RPX]] : (!emitrust.lvalue<i32>) -> i32
// CHECK-NEXT: %[[RPP1:.*]] = emitrust.deref %[[RPV]] : (!emitrust.ref<!emitrust.struct<"Point">>) -> !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK-NEXT: %[[RPY:.*]] = emitrust.member %[[RPP1]]["y"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: %[[RPYV:.*]] = emitrust.load %[[RPY]] : (!emitrust.lvalue<i32>) -> i32
// CHECK-NEXT: arith.addi %[[RPXV]], %[[RPYV]] : i32

// `Point&` -> a unique borrow of the struct; the member write is an
// ordinary `emitrust.assign` to the projected place.
// CHECK-LABEL: func.func @write_point
// CHECK-SAME: (%[[WPV:.*]]: !emitrust.mut_ref<!emitrust.struct<"Point">>)
// CHECK: %[[WPP:.*]] = emitrust.deref %[[WPV]] : (!emitrust.mut_ref<!emitrust.struct<"Point">>) -> !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK-NEXT: %[[WPX:.*]] = emitrust.member %[[WPP]]["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: %[[WPC:.*]] = arith.constant 3 : i32
// CHECK-NEXT: emitrust.assign %[[WPX]] = %[[WPC]] : !emitrust.lvalue<i32>

// A reference parameter alongside a method receiver. `add`: `mut_ref`
// receiver (non-const method) + `ref` parameter (`const int&`). The two
// derefs are of DIFFERENT arguments and neither is hoisted.
// CHECK: emitrust.struct_def @Accum ["total_"] [i32]
// CHECK-LABEL: func.func @Accum_add
// CHECK-SAME: (%[[ASELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Accum">>, %[[AV:.*]]: !emitrust.ref<i32>)
// CHECK-SAME: attributes {emitrust.method_of = "Accum"
// CHECK: %[[AVP:.*]] = emitrust.deref %[[AV]] : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
// CHECK-NEXT: %[[AVL:.*]] = emitrust.load %[[AVP]] : (!emitrust.lvalue<i32>) -> i32
// CHECK-NEXT: %[[ASUM:.*]] = arith.addi %{{.*}}, %[[AVL]] : i32
// CHECK-NEXT: emitrust.assign %{{.*}} = %[[ASUM]] : !emitrust.lvalue<i32>

// `take`: `mut_ref` receiver + `mut_ref` parameter, written through. The
// parameter's mutability comes from the PARAMETER (`Point&`), not from the
// method's constness -- these two happen to agree here, and `add` above is
// the case where they do not.
// CHECK-LABEL: func.func @Accum_take
// CHECK-SAME: (%[[TSELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Accum">>, %[[TP:.*]]: !emitrust.mut_ref<!emitrust.struct<"Point">>)
// CHECK-SAME: attributes {emitrust.method_of = "Accum"
// CHECK: %[[TPP:.*]] = emitrust.deref %[[TP]] : (!emitrust.mut_ref<!emitrust.struct<"Point">>) -> !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK-NEXT: %[[TPY:.*]] = emitrust.member %[[TPP]]["y"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>

// THE CALL SITES. Each argument is borrowed from the local's PLACE, with
// `is_mut` taken from the parameter: bare `addr_of` for `const T&`,
// `addr_of mut` for `T&`. The same local `%[[N]]` is borrowed shared and
// then uniquely at two different calls -- each borrow is single-use and
// dies at its call, which is what makes the sequence legal Rust.
// CHECK-LABEL: func.func @drive
// CHECK: %[[N:.*]] = emitrust.variable named "n" : !emitrust.lvalue<i32>
// CHECK: %[[PT:.*]] = emitrust.variable named "pt" : !emitrust.lvalue<!emitrust.struct<"Point">>

// `const int&` argument: SHARED borrow.
// CHECK: %[[BN:.*]] = emitrust.addr_of %[[N]] : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
// CHECK-NEXT: %{{.*}} = call @read_scalar(%[[BN]]) : (!emitrust.ref<i32>) -> i32

// `int&` argument: UNIQUE borrow of the very same place.
// CHECK: %[[BNM:.*]] = emitrust.addr_of mut %[[N]] : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
// CHECK-NEXT: call @write_scalar(%[[BNM]]) : (!emitrust.mut_ref<i32>) -> ()

// `const Point&` argument: shared borrow of a struct place.
// CHECK: %[[BP:.*]] = emitrust.addr_of %[[PT]] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.ref<!emitrust.struct<"Point">>
// CHECK-NEXT: %{{.*}} = call @read_point(%[[BP]]) : (!emitrust.ref<!emitrust.struct<"Point">>) -> i32

// `Point&` argument: unique borrow of the same struct place.
// CHECK: %[[BPM:.*]] = emitrust.addr_of mut %[[PT]] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.mut_ref<!emitrust.struct<"Point">>
// CHECK-NEXT: call @write_point(%[[BPM]]) : (!emitrust.mut_ref<!emitrust.struct<"Point">>) -> ()

// A METHOD call with a reference argument: TWO borrows are live at the same
// call, the receiver's and the argument's, and their mutabilities are
// independent. `add` is a non-const method taking `const int&`, so the
// receiver is `addr_of mut` and the argument is a plain `addr_of` — the
// clearest evidence that argument mutability comes from the PARAMETER and
// receiver mutability from the METHOD, not from one shared decision.
// CHECK: %[[MSELF:.*]] = emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"Accum">>) -> !emitrust.mut_ref<!emitrust.struct<"Accum">>
// CHECK-NEXT: %[[MARG:.*]] = emitrust.addr_of %[[N]] : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
// CHECK-NEXT: call @Accum_add(%[[MSELF]], %[[MARG]]) {emitrust.method_call} : (!emitrust.mut_ref<!emitrust.struct<"Accum">>, !emitrust.ref<i32>) -> ()

// `take` is a non-const method taking `Point&`: both borrows mutable, and
// they name DIFFERENT objects, so the same-object aliasing rule does not
// apply.
// CHECK: %[[TSELF2:.*]] = emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"Accum">>) -> !emitrust.mut_ref<!emitrust.struct<"Accum">>
// CHECK-NEXT: %[[TARG:.*]] = emitrust.addr_of mut %[[PT]] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.mut_ref<!emitrust.struct<"Point">>
// CHECK-NEXT: call @Accum_take(%[[TSELF2]], %[[TARG]]) {emitrust.method_call} : (!emitrust.mut_ref<!emitrust.struct<"Accum">>, !emitrust.mut_ref<!emitrust.struct<"Point">>) -> ()

// The receiver borrow of a const method is unchanged by any of the above:
// still a plain `addr_of` feeding an `emitrust.method_call`.
// CHECK: %[[BACC:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<!emitrust.struct<"Accum">>) -> !emitrust.ref<!emitrust.struct<"Accum">>
// CHECK-NEXT: %{{.*}} = call @Accum_total(%[[BACC]]) {emitrust.method_call} : (!emitrust.ref<!emitrust.struct<"Accum">>) -> i32
